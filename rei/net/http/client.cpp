#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/ascii.hpp"
#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/core/url.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/client.hpp"
#include "rei/net/http/connection.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::http::detail {
    struct pool {
        struct idle_connection {
            http::connection<net::socket> connection;
            std::chrono::steady_clock::time_point since;
        };

        std::mutex mutex;
        std::map<std::string, std::vector<idle_connection>, std::less<>> idle;
    };
} // namespace rei::net::http::detail

namespace rei::net::http {
    namespace {
        using deadline_type = std::optional<std::chrono::steady_clock::time_point>;

        constexpr auto copy_size = 16uz * 1024;

        auto failure(std::error_code code, std::string detail,
                     std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{code, std::move(detail), location});
        }

        struct destination {
            core::url url;
            std::string origin; // the pool's key
            std::string host; // the Host field
            std::string target;
            std::string address;
            std::uint16_t port{};
            bool ipv6{}; // a bracketed address in the URL
        };

        auto locate(std::string_view text) -> std::expected<destination, rei::error> {
            auto url = core::url::make(text);
            if (!url) {
                return std::unexpected(url.error());
            }
            if (url->scheme() != "http") {
                return failure(core::make_error_code(core::error_code::not_supported),
                               "only http URLs are supported");
            }
            if (!url->username().empty() || !url->password().empty()) {
                return failure(core::make_error_code(core::error_code::invalid_argument),
                               "credentials in URLs aren't supported");
            }

            const auto host = url->host().value_or("");
            if (host.empty()) {
                return failure(core::make_error_code(core::error_code::invalid_argument),
                               "the URL has no host");
            }

            const auto port = url->port().value_or(80);
            auto host_field = std::string{host};
            if (url->port()) {
                host_field += ':' + std::to_string(port);
            }

            auto target = std::string{url->path()};
            if (const auto query = url->query()) {
                target += '?';
                target += *query;
            }

            const auto ipv6 = host.starts_with('[');
            auto address = std::string{ipv6 ? host.substr(1, host.size() - 2) : host};

            return destination{
                .url = std::move(*url),
                .origin = std::string{host} + ':' + std::to_string(port),
                .host = std::move(host_field),
                .target = std::move(target),
                .address = std::move(address),
                .port = port,
                .ipv6 = ipv6,
            };
        }

        auto connect(const destination& where, std::optional<std::chrono::milliseconds> timeout,
                     deadline_type deadline) -> std::expected<net::socket, rei::error> {
            if (deadline) {
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now());
                if (left.count() <= 0) {
                    return failure(net::make_error_code(net::error_code::timed_out), "deadline passed");
                }

                timeout = timeout ? std::min(*timeout, left) : left;
            }

            // Names try IPv4 first, then IPv6. A failed connect is reported over a failed lookup.
            std::optional<rei::error> error;

            for (const auto domain : {net::communication_domain::ipv4, net::communication_domain::ipv6}) {
                if (where.ipv6 && domain == net::communication_domain::ipv4) {
                    continue;
                }

                auto endpoint = net::resolve({.address = where.address, .port = where.port}, domain,
                                             net::transport_protocol::tcp);
                if (!endpoint) {
                    error = error.value_or(endpoint.error());
                    continue;
                }

                auto socket = net::socket::make(domain, net::transport_protocol::tcp);
                if (!socket) {
                    return std::unexpected(socket.error());
                }

                auto connected = socket->set_timeout(timeout).and_then(
                    [&] -> std::expected<void, rei::error> { return socket->connect(*endpoint); });
                if (connected) {
                    return std::move(*socket);
                }

                // A timeout used up the time there was to connect.
                if (connected.error().code() == net::error_code::timed_out) {
                    return std::unexpected(connected.error());
                }

                error = connected.error();
            }

            return std::unexpected(*error);
        }

        auto take_idle(detail::pool& pool, std::string_view origin,
                       std::optional<std::chrono::milliseconds> idle_timeout)
            -> std::optional<http::connection<net::socket>> {
            const std::scoped_lock lock{pool.mutex};

            // Every origin is swept, or connections to one that is never asked for again would stay open.
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(pool.idle, [&](auto& entry) -> bool {
                std::erase_if(entry.second, [&](const detail::pool::idle_connection& idle) -> bool {
                    return idle_timeout &&
                           std::chrono::ceil<std::chrono::milliseconds>(now - idle.since) > *idle_timeout;
                });

                return entry.second.empty();
            });

            const auto found = pool.idle.find(origin);
            if (found == pool.idle.end()) {
                return std::nullopt;
            }

            auto& idle = found->second;
            while (!idle.empty()) {
                auto connection = std::move(idle.back().connection);
                idle.pop_back();

                if (!connection.stale()) {
                    return connection;
                }
            }

            return std::nullopt;
        }

        // Holds a connection while a response body streams from it.
        struct lease {
            std::shared_ptr<detail::pool> pool;
            std::string origin;
            std::size_t max_idle{};
            std::optional<http::connection<net::socket>> connection;

            auto release() -> void {
                if (connection && connection->reusable() && max_idle > 0) {
                    const std::scoped_lock lock{pool->mutex};

                    auto& idle = pool->idle[origin];
                    if (idle.size() == max_idle) {
                        idle.erase(idle.begin());
                    }

                    idle.push_back(
                        {.connection = std::move(*connection), .since = std::chrono::steady_clock::now()});
                }

                connection.reset();
            }
        };

        auto is_idempotent(std::string_view method) -> bool {
            return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE" ||
                   method == "PUT" || method == "DELETE";
        }

        auto is_framing_field(std::string_view name) -> bool {
            return core::ascii_iequals(name, "content-length") ||
                   core::ascii_iequals(name, "transfer-encoding");
        }

        auto send_request(http::connection<net::socket>& connection, const destination& where,
                          http::client_request& request) -> std::expected<void, rei::error> {
            http::request_head head{.method = request.method, .target = where.target};
            auto has_host = false;

            for (const auto& [name, value] : request.headers) {
                if (!is_framing_field(name)) {
                    has_host = has_host || core::ascii_iequals(name, "host");
                    head.headers.push_back({.name = name, .value = value});
                }
            }
            if (!has_host) {
                head.headers.push_back({.name = "Host", .value = where.host});
            }

            // RFC 9110 §8.6: no Content-Length on an empty body that the method doesn't expect.
            std::array<char, 24> length{};
            if (const auto size = request.body.size()) {
                if (*size > 0 || request.method == "POST" || request.method == "PUT" ||
                    request.method == "PATCH") {
                    const auto* const end =
                        std::to_chars(length.data(), length.data() + length.size(), *size).ptr;
                    head.headers.push_back(
                        {.name = "Content-Length", .value = std::string_view{length.data(), end}});
                }
            } else {
                head.headers.push_back({.name = "Transfer-Encoding", .value = "chunked"});
            }

            if (auto written = connection.write_head(head); !written) {
                return written;
            }

            if (!request.body.streamed()) {
                if (auto written = connection.write_body(request.body.bytes()); !written) {
                    return written;
                }

                return connection.finish();
            }

            std::vector<std::byte> buffer(copy_size);
            while (true) {
                const auto read = request.body.read(buffer);
                if (!read) {
                    return std::unexpected(read.error());
                }
                if (*read == 0) {
                    return connection.finish();
                }

                if (auto written = connection.write_body(std::span{buffer}.first(*read)); !written) {
                    return written;
                }
            }
        }

        auto receive_head(http::connection<net::socket>& connection, http::response_head& head,
                          std::string_view method) -> std::expected<void, rei::error> {
            while (true) {
                if (auto received = connection.read_head(head, method); !received) {
                    return received;
                }

                if (head.status == http::status::switching_protocols) {
                    return failure(core::make_error_code(core::error_code::not_supported),
                                   "protocol upgrades aren't supported");
                }
                if (std::to_underlying(head.status) >= 200) {
                    return {};
                }
            }
        }

        struct exchange {
            std::shared_ptr<lease> held;
            http::response response;
            h1::body_framing framing;
        };

        // Sends the request and reads the response head. A pooled connection the server has closed in the
        // meantime gets one retry on a new connection, when that is safe (RFC 9110 §9.2.2).
        auto perform(const http::client_options& options, const std::shared_ptr<detail::pool>& pool,
                     const destination& where, http::client_request& request, deadline_type deadline)
            -> std::expected<exchange, rei::error> {
            const h1::parse_limits limits{
                .max_head_size = options.max_head_size,
                .max_headers = options.max_headers,
            };
            const auto retryable = is_idempotent(request.method) && !request.body.streamed();

            for (auto fresh = false;; fresh = true) {
                auto held = std::make_shared<lease>(lease{
                    .pool = pool,
                    .origin = where.origin,
                    .max_idle = options.max_idle_per_host,
                    .connection = fresh ? std::nullopt : take_idle(*pool, where.origin, options.idle_timeout),
                });

                const auto reused = held->connection.has_value();
                if (!reused) {
                    auto socket = connect(where, options.connect_timeout, deadline);
                    if (!socket) {
                        return std::unexpected(socket.error());
                    }

                    held->connection.emplace(std::move(*socket), limits);
                }

                auto& connection = *held->connection;
                connection.deadline(deadline);
                if (auto limited = connection.timeout(options.io_timeout); !limited) {
                    return std::unexpected(limited.error());
                }

                http::response_head head;
                auto received =
                    send_request(connection, where, request).and_then([&] -> std::expected<void, rei::error> {
                        return receive_head(connection, head, request.method);
                    });
                if (!received) {
                    if (reused && retryable &&
                        received.error().code() == net::error_code::connection_closed) {
                        continue;
                    }

                    return std::unexpected(received.error());
                }

                exchange result{
                    .held = std::move(held),
                    .response = {.status = head.status},
                    .framing = h1::response_framing(head, request.method).value_or(h1::body_framing{}),
                };
                for (const auto& field : head.headers) {
                    result.response.headers.add(field.name, field.value);
                }

                return result;
            }
        }

        auto is_redirect(http::status status) -> bool {
            return status == http::status::moved_permanently || status == http::status::found ||
                   status == http::status::see_other || status == http::status::temporary_redirect ||
                   status == http::status::permanent_redirect;
        }

        // Rewrites `request` for the next hop the way fetch does. False when it can't be sent again.
        auto redirect(http::client_request& request, http::status status, const core::url& from,
                      const core::url& next) -> bool {
            const auto to_get =
                (status == http::status::see_other && request.method != "HEAD") ||
                ((status == http::status::moved_permanently || status == http::status::found) &&
                 request.method == "POST");
            if (!to_get && request.body.streamed()) {
                return false;
            }

            if (to_get) {
                request.method = "GET";
                request.body = {};
                for (const auto* const name :
                     {"Content-Type", "Content-Encoding", "Content-Language", "Content-Location"}) {
                    request.headers.remove(name);
                }
            }

            if (from.scheme() != next.scheme() || from.host() != next.host() || from.port() != next.port()) {
                for (const auto* const name : {"Authorization", "Cookie", "Host", "Proxy-Authorization"}) {
                    request.headers.remove(name);
                }
            }

            request.url = next.serialize();
            return true;
        }

        auto streamed_body(std::shared_ptr<lease> held, h1::body_framing framing) -> http::body {
            const auto size =
                framing.kind == h1::body_kind::sized ? std::optional{framing.size} : std::nullopt;

            auto reader =
                [held = std::move(held)](std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
                if (!held->connection) {
                    return 0;
                }

                auto read = held->connection->read_body(out);
                if (!read) {
                    held->connection.reset();
                } else if (*read == 0) {
                    held->release();
                }

                return read;
            };

            return http::body{std::move(reader), size};
        }
    } // namespace

    client::client(http::client_options options)
        : m_options(std::move(options)), m_pool(std::make_shared<detail::pool>()) {}

    auto client::_open(http::client_request request, deadline_type deadline, bool deadline_covers_body)
        -> std::expected<http::response, rei::error> {
        // Merged up front, so a redirect drops a default field the same way as one of the request's own.
        const auto own = request.headers;
        for (const auto& [name, value] : m_options.headers) {
            if (!own.find(name)) {
                request.headers.add(name, value);
            }
        }

        for (auto redirects = 0u;; redirects++) {
            auto where = locate(request.url);
            if (!where) {
                return std::unexpected(where.error());
            }

            auto performed = perform(m_options, m_pool, *where, request, deadline);
            if (!performed) {
                return std::unexpected(performed.error());
            }

            auto& [held, response, framing] = *performed;

            const auto location = response.headers.find("Location");
            if (m_options.max_redirects > 0 && is_redirect(response.status) && location) {
                if (redirects == m_options.max_redirects) {
                    return failure(http::make_error_code(http::error_code::too_many_redirects),
                                   "too many redirects");
                }

                auto next = core::url::make(*location, where->url);
                if (!next) {
                    return std::unexpected(next.error());
                }
                if (redirect(request, response.status, where->url, *next)) {
                    continue;
                }
            }

            if (!deadline_covers_body) {
                held->connection->deadline(std::nullopt);
            }

            if (framing.kind == h1::body_kind::none ||
                (framing.kind == h1::body_kind::sized && framing.size == 0)) {
                held->release();
            } else {
                response.body = streamed_body(std::move(held), framing);
            }

            return std::move(response);
        }
    }

    auto client::send(http::client_request request) -> std::expected<http::response, rei::error> {
        auto response = _open(std::move(request), m_options.timeout.transform(http::deadline_after), true);
        if (!response || !response->body.streamed()) {
            return response;
        }

        const auto limit = m_options.max_body_size;
        const auto size = response->body.size();
        if (limit && size && *size > *limit) {
            return failure(http::make_error_code(http::error_code::body_too_large),
                           "response body exceeds max_body_size");
        }

        std::vector<std::byte> bytes;
        auto filled = 0uz;
        while (true) {
            if (filled == bytes.size()) {
                // The size a server declares is trusted up to the limit. Without one, the buffer only
                // grows with what has arrived.
                const auto initial = limit && size ? static_cast<std::size_t>(*size) + 1 : copy_size;
                bytes.resize(std::max(bytes.size() * 2, initial));
            }

            const auto read = response->body.read(std::span{bytes}.subspan(filled));
            if (!read) {
                return std::unexpected(read.error());
            }
            if (*read == 0) {
                break;
            }

            filled += *read;
            if (limit && filled > *limit) {
                return failure(http::make_error_code(http::error_code::body_too_large),
                               "response body exceeds max_body_size");
            }
        }

        bytes.resize(filled);
        response->body = http::body{std::move(bytes)};

        return response;
    }

    auto client::stream(http::client_request request) -> std::expected<http::response, rei::error> {
        return _open(std::move(request), m_options.timeout.transform(http::deadline_after), false);
    }
} // namespace rei::net::http
