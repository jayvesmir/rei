#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rei/core/ascii.hpp"
#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/connection.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/http/server.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::http {
    struct server::session {
        http::connection connection;
        bool idle{};
    };

    namespace {
        using namespace std::chrono_literals;

        constexpr auto accept_wait = 250ms;
        constexpr auto accept_retry_delay = 100ms;
        constexpr auto drain_limit = 256uz * 1024;
        constexpr auto copy_size = 16uz * 1024;
        constexpr auto linger_wait = 100ms;
        constexpr auto linger_limit = 64uz * 1024;

        auto failure(http::error_code code, std::string detail,
                     std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{http::make_error_code(code), std::move(detail), location});
        }

        auto is_http_error(const std::error_code& code) -> bool {
            return code.category() == http::make_error_code(http::error_code::bad_header).category();
        }

        // The peer left or went quiet, which happens and isn't worth a report.
        auto is_peer_failure(const std::error_code& code) -> bool {
            return code == net::error_code::connection_closed || code == net::error_code::timed_out;
        }

        // std::nullopt when the peer is gone and nothing should be sent.
        auto error_status(const rei::error& error) -> std::optional<http::status> {
            const auto code = error.code();

            if (code == net::error_code::connection_closed) {
                return std::nullopt;
            }
            if (code == net::error_code::timed_out) {
                return http::status::request_timeout;
            }
            if (!is_http_error(code)) {
                return http::status::internal_server_error;
            }

            switch (static_cast<http::error_code>(code.value())) {
            case http::error_code::body_too_large:
                return http::status::content_too_large;
            case http::error_code::head_too_large:
            case http::error_code::too_many_headers:
                return http::status::request_header_fields_too_large;
            case http::error_code::unsupported_transfer_coding:
                return http::status::not_implemented;
            case http::error_code::unsupported_version:
                return http::status::http_version_not_supported;
            default:
                return http::status::bad_request;
            }
        }

        // RFC 9110 §5.6.7, cached per thread for the current second.
        auto date_text() -> std::string_view {
            thread_local std::chrono::sys_seconds cached{};
            thread_local std::string text;

            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            if (now != cached || text.empty()) {
                cached = now;
                text = std::format("{:%a, %d %b %Y %H:%M:%S} GMT", now);
            }

            return text;
        }

        // "." or "..", also spelled with %2E.
        auto is_dot_segment(std::string_view segment) -> bool {
            auto dots = 0uz;

            while (!segment.empty()) {
                if (segment.front() == '.') {
                    segment.remove_prefix(1);
                } else if (segment.size() >= 3 && segment.starts_with("%2") &&
                           core::to_ascii_lower(segment[2]) == 'e') {
                    segment.remove_prefix(3);
                } else {
                    return false;
                }

                dots++;
            }

            return dots == 1 || dots == 2;
        }

        struct target_parts {
            std::string_view path;
            std::optional<std::string_view> query;
        };

        // Origin-form, absolute-form, or `*` for OPTIONS (RFC 9112 §3.2).
        auto split_target(std::string_view method, std::string_view target) -> std::optional<target_parts> {
            if (target == "*") {
                return method == "OPTIONS" ? std::optional{target_parts{.path = target}} : std::nullopt;
            }

            if (!target.starts_with('/')) {
                const auto scheme_end = target.find("://");
                if (scheme_end == 0 || scheme_end == std::string_view::npos) {
                    return std::nullopt;
                }

                const auto path_start = target.find_first_of("/?", scheme_end + 3);
                if (path_start == std::string_view::npos) {
                    return target_parts{.path = "/"};
                }
                if (target[path_start] == '?') {
                    return target_parts{.path = "/", .query = target.substr(path_start + 1)};
                }

                target.remove_prefix(path_start);
            }

            const auto query_start = target.find('?');
            if (query_start == std::string_view::npos) {
                return target_parts{.path = target};
            }

            return target_parts{
                .path = target.substr(0, query_start),
                .query = target.substr(query_start + 1),
            };
        }

        auto write_response(http::connection& connection, const http::request_head& request,
                            http::response& response, bool closing, http::response_head& head,
                            std::vector<std::byte>& buffer) -> std::expected<void, rei::error> {
            const auto is_head = request.method == http::to_string(http::method::head);
            const auto bodiless =
                response.status == http::status::no_content || response.status == http::status::not_modified;

            head.version = http::version::http_1_1;
            head.status = response.status;
            head.reason = {};
            head.headers.clear();

            auto has_date = false;
            for (const auto& [name, value] : response.headers) {
                const http::header field{.name = name, .value = value};

                if (core::ascii_iequals(name, "content-length") ||
                    core::ascii_iequals(name, "transfer-encoding")) {
                    continue;
                }
                if (core::ascii_iequals(name, "connection")) {
                    closing = closing || !h1::keeps_alive(http::version::http_1_1, std::span{&field, 1});
                    continue;
                }

                has_date = has_date || core::ascii_iequals(name, "date");
                head.headers.push_back(field);
            }
            if (!has_date) {
                head.headers.push_back({.name = "Date", .value = date_text()});
            }

            std::array<char, 24> length{};
            if (!bodiless) {
                if (const auto size = response.body.size()) {
                    const auto* const end =
                        std::to_chars(length.data(), length.data() + length.size(), *size).ptr;
                    head.headers.push_back(
                        {.name = "Content-Length", .value = std::string_view{length.data(), end}});
                } else if (request.version == http::version::http_1_1) {
                    head.headers.push_back({.name = "Transfer-Encoding", .value = "chunked"});
                } else if (!is_head) {
                    closing = true;
                }
            }

            if (closing) {
                head.headers.push_back({.name = "Connection", .value = "close"});
            } else if (request.version == http::version::http_1_0) {
                head.headers.push_back({.name = "Connection", .value = "keep-alive"});
            }

            if (auto written = connection.write_head(head, request.method); !written) {
                return written;
            }

            if (!is_head && !bodiless) {
                if (!response.body.streamed()) {
                    if (auto written = connection.write_body(response.body.bytes()); !written) {
                        return written;
                    }
                }

                while (response.body.streamed()) {
                    const auto read = response.body.read(buffer);
                    if (!read) {
                        return std::unexpected(read.error());
                    }
                    if (*read == 0) {
                        break;
                    }

                    if (auto written = connection.write_body(std::span{buffer}.first(*read)); !written) {
                        return written;
                    }
                }
            }

            return connection.finish();
        }

        // Gives the peer a moment to read the response before unread input makes the close a reset.
        auto linger(net::socket& socket) -> void {
            if (!socket.shutdown(net::shutdown_mode::send) || !socket.set_timeout(linger_wait)) {
                return;
            }

            std::array<std::byte, 4096> sink{};
            for (auto discarded = 0uz; discarded < linger_limit;) {
                const auto received = socket.receive(sink);
                if (!received) {
                    return;
                }

                discarded += *received;
            }
        }
    } // namespace

    auto request::_start() -> std::optional<http::status> {
        m_params.clear();
        m_decoded.clear();
        m_body.clear();
        m_body_buffered = false;
        m_body_failed = false;
        m_body_received = 0;
        m_continue_pending = false;

        if (m_head.method == http::to_string(http::method::connect)) {
            return http::status::not_implemented;
        }

        const auto hosts = std::ranges::count_if(m_head.headers, [](const http::header& header) -> bool {
            return core::ascii_iequals(header.name, "host");
        });
        if (hosts > 1 || (hosts == 0 && m_head.version == http::version::http_1_1)) {
            return http::status::bad_request;
        }

        const auto target = split_target(m_head.method, m_head.target);
        if (!target ||
            std::ranges::any_of(std::views::split(target->path, '/'), [](const auto& segment) -> bool {
                return is_dot_segment(std::string_view{segment});
            })) {
            return http::status::bad_request;
        }

        m_path = target->path;
        m_query = target->query;

        const auto framing = h1::request_framing(m_head).value_or(h1::body_framing{});
        m_body_size = framing.kind == h1::body_kind::chunked ? std::nullopt : std::optional{framing.size};

        // RFC 9110 §10.1.1: an HTTP/1.0 server ignores 100-continue.
        const auto expect = header("expect");
        if (expect && m_head.version == http::version::http_1_1) {
            if (!core::ascii_iequals(*expect, "100-continue")) {
                return http::status::expectation_failed;
            }

            m_continue_pending = m_body_size != 0;
        }

        return std::nullopt;
    }

    auto request::read_body(std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
        if (m_continue_pending) {
            m_continue_pending = false;

            const http::response_head interim{.status = http::status::continue_};
            auto answered =
                m_connection->write_head(interim, m_head.method)
                    .and_then([this] -> std::expected<void, rei::error> { return m_connection->finish(); });
            if (!answered) {
                return std::unexpected(answered.error());
            }
        }

        auto read = m_connection->read_body(out);
        if (read) {
            m_body_received += *read;
        } else {
            m_body_failed = true;
        }

        return read;
    }

    auto request::body() -> std::expected<std::span<const std::byte>, rei::error> {
        if (m_body_buffered) {
            return m_body;
        }

        const auto too_large = [this] -> std::unexpected<rei::error> {
            m_body_failed = true;
            return failure(http::error_code::body_too_large, "request body exceeds max_body_size");
        };
        if (m_body_size && m_max_body_size && *m_body_size - m_body_received > *m_max_body_size) {
            return too_large();
        }

        auto filled = 0uz;
        while (true) {
            if (filled == m_body.size()) {
                m_body.resize(std::max(m_body.size() * 2, copy_size));
            }

            const auto read = read_body(std::span{m_body}.subspan(filled));
            if (!read) {
                return std::unexpected(read.error());
            }
            if (*read == 0) {
                break;
            }

            filled += *read;
            if (m_max_body_size && filled > *m_max_body_size) {
                return too_large();
            }
        }

        m_body.resize(filled);
        m_body_buffered = true;

        return m_body;
    }

    auto request::text() -> std::expected<std::string_view, rei::error> {
        return body().transform([](std::span<const std::byte> bytes) -> std::string_view {
            return std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        });
    }

    // Reads what the handler left of the body, so the connection can carry the next request.
    auto request::_drain() -> bool {
        // The client still waits for 100 Continue and hasn't sent the body.
        if (m_continue_pending) {
            return false;
        }
        if (m_body_size && *m_body_size - m_body_received > drain_limit) {
            return false;
        }

        std::array<std::byte, 4096> sink;
        for (auto drained = 0uz; drained <= drain_limit;) {
            const auto read = m_connection->read_body(sink);
            if (!read) {
                return false;
            }
            if (*read == 0) {
                return true;
            }

            drained += *read;
        }

        return false;
    }

    server::server(http::handler handler, http::server_options options)
        : m_handler(std::move(handler)), m_options(std::move(options)) {}

    auto server::_limits() const -> h1::parse_limits {
        return {
            .max_head_size = m_options.max_head_size,
            .max_headers = m_options.max_headers,
        };
    }

    auto server::_report(const rei::error& error) const -> void {
        if (m_options.on_error) {
            m_options.on_error(error);
        }
    }

    auto server::listen(const net::ip_endpoint& local) -> std::expected<net::ip_endpoint, rei::error> {
        const auto domain =
            local.address.contains(':') ? net::communication_domain::ipv6 : net::communication_domain::ipv4;

        auto socket = net::socket::make(domain, net::transport_protocol::tcp);
        if (!socket) {
            return std::unexpected(socket.error());
        }

        auto bound = socket->set_reuse_address(true)
                         .and_then([&] -> std::expected<void, rei::error> { return socket->bind(local); })
                         .and_then([&] -> std::expected<void, rei::error> { return socket->listen(); })
                         .and_then([&] -> std::expected<net::ip_endpoint, rei::error> {
                             return socket->local_endpoint();
                         });
        if (!bound) {
            return std::unexpected(bound.error());
        }

        m_listener = std::move(*socket);
        return bound;
    }

    auto server::_register(server::session* session) -> void {
        const std::scoped_lock lock{m_mutex};
        m_sessions.push_back(session);
    }

    auto server::_unregister(server::session* session) -> void {
        const std::scoped_lock lock{m_mutex};
        std::erase(m_sessions, session);
        m_changed.notify_all();
    }

    auto server::_spawn(net::socket socket) -> void {
        auto session = std::make_unique<server::session>(http::connection{std::move(socket), _limits()});
        auto* const registered = session.get();

        // Registered before the thread starts, so run() can't miss it while waiting for sessions to end.
        _register(registered);

        try {
            std::thread{[this, session = std::move(session)] -> void {
                _serve(*session);
                _unregister(session.get());
            }}.detach();
        } catch (const std::system_error& error) {
            _unregister(registered);
            _report(rei::error{error.code(), "failed to start a connection thread"});
        }
    }

    auto server::serve(net::socket socket) -> void {
        server::session session{.connection = http::connection{std::move(socket), _limits()}};

        _register(&session);
        _serve(session);
        _unregister(&session);
    }

    auto server::run() -> std::expected<void, rei::error> {
        if (!m_listener.is_open()) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "run() needs a successful listen() first"});
        }
        if (auto set = m_listener.set_timeout(accept_wait); !set) {
            return set;
        }

        while (!m_stopping) {
            if (m_options.max_connections) {
                std::unique_lock lock{m_mutex};
                const auto has_room = m_changed.wait_for(lock, accept_wait, [this] -> bool {
                    return m_sessions.size() < *m_options.max_connections || m_stopping;
                });
                if (!has_room || m_stopping) {
                    continue;
                }
            }

            auto accepted = m_listener.accept();
            if (!accepted) {
                if (accepted.error().code() != net::error_code::timed_out) {
                    _report(accepted.error());
                    std::this_thread::sleep_for(accept_retry_delay);
                }

                continue;
            }

            _spawn(std::move(*accepted));
        }

        (void)m_listener.close();

        std::unique_lock lock{m_mutex};
        for (auto* const session : m_sessions) {
            if (session->idle) {
                (void)session->connection.socket().shutdown(net::shutdown_mode::both);
            }
        }

        m_changed.wait(lock, [this] -> bool { return m_sessions.empty(); });
        return {};
    }

    auto server::_serve(server::session& session) -> void {
        auto& connection = session.connection;

        http::request request;
        request.m_connection = &connection;
        request.m_max_body_size = m_options.max_body_size;
        request.m_peer = connection.socket().peer_endpoint().value_or(net::ip_endpoint{});

        http::response_head head;
        std::vector<std::byte> buffer(copy_size);

        const auto reject = [&](http::status status, const http::request_head& request_head) -> void {
            http::response response{.status = status};
            (void)write_response(connection, request_head, response, true, head, buffer);
        };

        while (true) {
            {
                const std::scoped_lock lock{m_mutex};
                if (m_stopping) {
                    break;
                }

                session.idle = true;
            }

            connection.deadline(m_options.head_timeout.transform(http::deadline_after));
            connection.timeout(std::nullopt);
            const auto received = connection.read_head(request.m_head);

            {
                const std::scoped_lock lock{m_mutex};
                session.idle = false;
            }

            if (!received) {
                if (is_http_error(received.error().code())) {
                    reject(*error_status(received.error()), http::request_head{});
                } else if (!is_peer_failure(received.error().code())) {
                    _report(received.error());
                }

                break;
            }

            connection.deadline(std::nullopt);
            connection.timeout(m_options.io_timeout);

            if (const auto rejected = request._start()) {
                reject(*rejected, request.m_head);
                break;
            }

            auto closing = !h1::keeps_alive(request.m_head.version, request.m_head.headers);

            auto result = m_handler(request);
            if (result && std::to_underlying(result->status) < 200) {
                result = std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                    "a handler returned an interim status"});
            }

            http::response response;
            if (result) {
                response = std::move(*result);
            } else {
                // Only a failed read of the request's body says something about the client. The same
                // code from elsewhere, such as a request the handler made, is the handler's failure.
                const auto status = request.m_body_failed ? error_status(result.error())
                                                          : http::status::internal_server_error;
                if (!status) {
                    break;
                }
                if (*status == http::status::internal_server_error) {
                    _report(result.error());
                }

                // A failed body read leaves the connection in an unknown state.
                closing = closing || *status != http::status::internal_server_error;
                response = http::response{.status = *status};
            }

            // A stop() during the handler only shows now.
            closing = closing || m_stopping || !request._drain();

            if (auto written = write_response(connection, request.m_head, response, closing, head, buffer);
                !written) {
                if (!is_peer_failure(written.error().code())) {
                    _report(written.error());

                    // A head that was refused has sent nothing, so the client can still be answered. Past
                    // the head, the connection won't start another message and this does nothing.
                    reject(http::status::internal_server_error, request.m_head);
                }

                break;
            }

            if (closing || !connection.reusable()) {
                break;
            }
        }

        linger(connection.socket());
    }
} // namespace rei::net::http
