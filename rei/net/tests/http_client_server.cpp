#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/client.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/server.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    using namespace std::chrono_literals;

    namespace {
        class test_server {
          private:
            http::server m_server;
            net::ip_endpoint m_local;
            std::jthread m_thread;

          public:
            explicit test_server(http::handler handler, http::server_options options = {})
                : m_server(std::move(handler), std::move(options)),
                  m_local(m_server.listen({.address = "127.0.0.1", .port = 0}).value()),
                  m_thread([this] -> void { (void)m_server.run(); }) {}

            test_server(const test_server&) = delete;
            auto operator=(const test_server&) -> test_server& = delete;

            ~test_server() { m_server.stop(); }

            [[nodiscard]] auto port() const -> std::uint16_t { return m_local.port; }
            [[nodiscard]] auto url(std::string_view path) const -> std::string {
                return std::format("http://127.0.0.1:{}{}", m_local.port, path);
            }
        };

        auto connect_to(std::uint16_t port) -> std::expected<net::socket, rei::error> {
            auto socket = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
            if (!socket) {
                return socket;
            }

            return socket->set_timeout(5s)
                .and_then([&] -> std::expected<void, rei::error> {
                    return socket->connect({.address = "127.0.0.1", .port = port});
                })
                .transform([&] -> net::socket { return std::move(*socket); });
        }

        auto listen_locally() -> std::expected<net::socket, rei::error> {
            auto listener = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
            if (!listener) {
                return listener;
            }

            return listener->set_timeout(5s)
                .and_then([&] -> std::expected<void, rei::error> {
                    return listener->bind(tests::loopback(net::communication_domain::ipv4));
                })
                .and_then([&] -> std::expected<void, rei::error> { return listener->listen(); })
                .transform([&] -> net::socket { return std::move(*listener); });
        }

        auto send_text(net::socket& socket, std::string_view text) -> std::expected<void, rei::error> {
            while (!text.empty()) {
                const auto sent = socket.send(tests::as_bytes(text));
                if (!sent) {
                    return std::unexpected(sent.error());
                }

                text.remove_prefix(*sent);
            }

            return {};
        }

        // Everything until the server closes, or until the text ends with `end`.
        auto receive_text(net::socket& socket, std::string_view end = {})
            -> std::expected<std::string, rei::error> {
            std::string received;
            std::array<std::byte, 4096> buffer{};

            while (end.empty() || !received.ends_with(end)) {
                const auto read = socket.receive(buffer);
                if (!read) {
                    if (read.error().code() == net::error_code::connection_closed) {
                        return received;
                    }

                    return std::unexpected(read.error());
                }

                received += tests::as_text(std::span{buffer}.first(*read));
            }

            return received;
        }

        auto exchange_raw(std::uint16_t port, std::string_view request)
            -> std::expected<std::string, rei::error> {
            auto socket = connect_to(port);
            if (!socket) {
                return std::unexpected(socket.error());
            }

            return send_text(*socket, request).and_then([&] -> std::expected<std::string, rei::error> {
                return receive_text(*socket);
            });
        }

        auto read_all(http::body& body) -> std::expected<std::string, rei::error> {
            std::array<std::byte, 2> buffer{};
            std::string text;

            while (true) {
                const auto read = body.read(buffer);
                if (!read) {
                    return std::unexpected(read.error());
                }
                if (*read == 0) {
                    return text;
                }

                text += tests::as_text(std::span{buffer}.first(*read));
            }
        }
    } // namespace

    TEST(http_client_server, router_prefers_the_first_literal_from_the_left) {
        http::router router;
        ASSERT_TRUE(router.route("/{lang}/docs", [](http::request& request) -> http::response {
            return {.body = std::format("docs in {}", request.param("lang"))};
        }));
        ASSERT_TRUE(router.route("/api/{rest...}", [](http::request& request) -> http::response {
            return {.body = std::format("api {}", request.param("rest"))};
        }));
        ASSERT_TRUE(
            router.route("GET /users/me", [](http::request&) -> http::response { return {.body = "me"}; }));
        ASSERT_TRUE(router.route("GET /users/{id}", [](http::request& request) -> http::response {
            return {.body = std::format("user {}", request.param("id"))};
        }));

        EXPECT_TRUE(tests::has_error(router.route("GET /users/{key}", {}), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(router.route("/files/{path...}/more", {}), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(router.route("/a{b}", {}), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(router.route("users", {}), std::errc::invalid_argument));

        const test_server server{router};
        http::client client;
        const auto body_of = [&](std::string_view path) -> std::string {
            auto response = client.send({.url = server.url(path)});
            return response ? std::string{response->body.text()} : "failed";
        };

        EXPECT_EQ(body_of("/api/docs"), "api docs");
        EXPECT_EQ(body_of("/en/docs"), "docs in en");
        EXPECT_EQ(body_of("/api/v1/items/"), "api v1/items/");
        EXPECT_EQ(body_of("/users/me"), "me");
        EXPECT_EQ(body_of("/users/caf%C3%A9"), "user café");
    }

    TEST(http_client_server, router_answers_unknown_paths_wrong_methods_and_head) {
        http::router router;
        ASSERT_TRUE(
            router.route("GET /items", [](http::request&) -> http::response { return {.body = "items"}; }));
        ASSERT_TRUE(router.route("POST /items", [](http::request&) -> http::response {
            return {.status = http::status::created};
        }));
        ASSERT_TRUE(router.route("/", [](http::request&) -> http::response { return {.body = "root"}; }));

        const test_server server{router};
        http::client client;

        const auto missing = client.send({.url = server.url("/nothing")});
        ASSERT_TRUE(missing) << missing.error();
        EXPECT_EQ(missing->status, http::status::not_found);

        // The `*` target is not a path, so not even "/" matches it.
        const auto asterisk =
            exchange_raw(server.port(), "OPTIONS * HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
        ASSERT_TRUE(asterisk) << asterisk.error();
        EXPECT_TRUE(asterisk->starts_with("HTTP/1.1 404 Not Found\r\n"));

        const auto wrong = client.send({.method = "DELETE", .url = server.url("/items")});
        ASSERT_TRUE(wrong) << wrong.error();
        EXPECT_EQ(wrong->status, http::status::method_not_allowed);
        EXPECT_EQ(wrong->headers.find("allow"), "GET, HEAD, POST");

        const auto head = client.send({.method = "HEAD", .url = server.url("/items")});
        ASSERT_TRUE(head) << head.error();
        EXPECT_EQ(head->status, http::status::ok);
        EXPECT_EQ(head->headers.find("Content-Length"), "5");
        EXPECT_TRUE(head->body.text().empty());

        const auto created = client.send({.method = "POST", .url = server.url("/items")});
        ASSERT_TRUE(created) << created.error();
        EXPECT_EQ(created->status, http::status::created);
        EXPECT_EQ(created->headers.find("Content-Length"), "0");
        EXPECT_TRUE(created->headers.find("Date"));
    }

    TEST(http_client_server, connections_are_reused_and_bodies_stream) {
        http::router router;
        ASSERT_TRUE(router.route("GET /port", [](http::request& request) -> http::response {
            return {.body = std::to_string(request.peer().port)};
        }));
        ASSERT_TRUE(router.route("GET /stream", [](http::request&) -> http::response {
            auto left = 3;
            return {.body = http::body{
                        [left](std::span<std::byte> out) mutable -> std::expected<std::size_t, rei::error> {
                            if (left == 0) {
                                return 0;
                            }

                            left--;
                            out[0] = std::byte{'x'};
                            return 1;
                        }}};
        }));

        const test_server server{router};
        http::client client;

        const auto first = client.send({.url = server.url("/port")});
        const auto second = client.send({.url = server.url("/port")});
        ASSERT_TRUE(first && second);
        EXPECT_EQ(first->body.text(), second->body.text());

        auto streamed = client.stream({.url = server.url("/stream")});
        ASSERT_TRUE(streamed) << streamed.error();
        EXPECT_TRUE(streamed->body.streamed());
        EXPECT_FALSE(streamed->body.size());
        EXPECT_EQ(streamed->headers.find("Transfer-Encoding"), "chunked");

        const auto text = read_all(streamed->body);
        ASSERT_TRUE(text) << text.error();
        EXPECT_EQ(*text, "xxx");

        const auto third = client.send({.url = server.url("/port")});
        ASSERT_TRUE(third) << third.error();
        EXPECT_EQ(third->body.text(), first->body.text());
    }

    TEST(http_client_server, request_bodies_are_read_on_demand_and_limited) {
        std::mutex mutex;
        std::vector<std::string> errors;

        http::router router;
        ASSERT_TRUE(router.route(
            "POST /echo", [](http::request& request) -> std::expected<http::response, rei::error> {
                return request.text().transform(
                    [](std::string_view text) -> http::response { return {.body = text}; });
            }));
        ASSERT_TRUE(
            router.route("POST /fail", [](http::request&) -> std::expected<http::response, rei::error> {
                // The code of a failed body read, but from elsewhere, such as a request to another server.
                return std::unexpected(rei::error{net::make_error_code(net::error_code::timed_out), "boom"});
            }));

        const test_server server{router,
                                 {
                                     .max_body_size = 8,
                                     .on_error = [&](const rei::error& error) -> void {
                                         const std::scoped_lock lock{mutex};
                                         errors.push_back(error.detail());
                                     },
                                 }};
        http::client client;

        const auto echoed = client.send({.method = "POST", .url = server.url("/echo"), .body = "hello"});
        ASSERT_TRUE(echoed) << echoed.error();
        EXPECT_EQ(echoed->body.text(), "hello");

        auto sent = false;
        const auto chunked = client.send({
            .method = "POST",
            .url = server.url("/echo"),
            .body = http::body{[&sent](std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
                if (sent) {
                    return 0;
                }

                sent = true;
                out[0] = std::byte{'a'};
                out[1] = std::byte{'b'};
                return 2;
            }},
        });
        ASSERT_TRUE(chunked) << chunked.error();
        EXPECT_EQ(chunked->body.text(), "ab");

        const auto too_large =
            client.send({.method = "POST", .url = server.url("/echo"), .body = "longer than eight"});
        ASSERT_TRUE(too_large) << too_large.error();
        EXPECT_EQ(too_large->status, http::status::content_too_large);

        const auto failed = client.send({.method = "POST", .url = server.url("/fail")});
        ASSERT_TRUE(failed) << failed.error();
        EXPECT_EQ(failed->status, http::status::internal_server_error);

        const std::scoped_lock lock{mutex};
        EXPECT_EQ(errors, std::vector<std::string>{"boom"});
    }

    TEST(http_client_server, expect_continue_is_answered_on_the_first_body_read) {
        http::router router;
        ASSERT_TRUE(router.route(
            "POST /read", [](http::request& request) -> std::expected<http::response, rei::error> {
                return request.text().transform(
                    [](std::string_view text) -> http::response { return {.body = text}; });
            }));
        ASSERT_TRUE(router.route("POST /ignore", [](http::request&) -> http::response {
            return {.status = http::status::forbidden};
        }));

        const test_server server{router};
        auto socket = connect_to(server.port());
        ASSERT_TRUE(socket) << socket.error();

        ASSERT_TRUE(send_text(*socket, "POST /read HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                                       "Expect: 100-continue\r\n\r\n"));
        const auto interim = receive_text(*socket, "\r\n\r\n");
        ASSERT_TRUE(interim) << interim.error();
        EXPECT_EQ(*interim, "HTTP/1.1 100 Continue\r\n\r\n");

        ASSERT_TRUE(send_text(*socket, "hello"));
        const auto echoed = receive_text(*socket, "\r\n\r\nhello");
        ASSERT_TRUE(echoed) << echoed.error();
        EXPECT_TRUE(echoed->starts_with("HTTP/1.1 200 OK\r\n"));

        ASSERT_TRUE(send_text(*socket, "POST /ignore HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                                       "Expect: 100-continue\r\n\r\n"));
        const auto refused = receive_text(*socket);
        ASSERT_TRUE(refused) << refused.error();
        EXPECT_TRUE(refused->starts_with("HTTP/1.1 403 Forbidden\r\n"));
        EXPECT_TRUE(refused->contains("Connection: close\r\n"));
    }

    TEST(http_client_server, malformed_requests_get_an_error_and_a_closed_connection) {
        const test_server server{[](http::request& request) -> http::response {
            return {.body = std::format("{}?{}", request.path(), request.query().value_or(""))};
        }};
        const auto status_line = [&](std::string_view request) -> std::string {
            const auto response = exchange_raw(server.port(), request);
            return response ? response->substr(0, response->find('\r')) : "failed";
        };

        EXPECT_EQ(status_line("GET / HTTP/1.1\r\n\r\n"), "HTTP/1.1 400 Bad Request");
        EXPECT_EQ(status_line("GET /a/../b HTTP/1.1\r\nHost: a\r\n\r\n"), "HTTP/1.1 400 Bad Request");
        EXPECT_EQ(status_line("GET /a/%2e%2E/b HTTP/1.1\r\nHost: a\r\n\r\n"), "HTTP/1.1 400 Bad Request");
        EXPECT_EQ(status_line("BROKEN\r\n\r\n"), "HTTP/1.1 400 Bad Request");
        EXPECT_EQ(status_line("GET / HTTP/1.1\r\nHost: a\r\nExpect: magic\r\n\r\n"),
                  "HTTP/1.1 417 Expectation Failed");
        EXPECT_EQ(status_line("GET / HTTP/2.0\r\nHost: a\r\n\r\n"),
                  "HTTP/1.1 505 HTTP Version Not Supported");

        const auto absolute =
            exchange_raw(server.port(), "GET http://a/x?y HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
        ASSERT_TRUE(absolute) << absolute.error();
        EXPECT_TRUE(absolute->starts_with("HTTP/1.1 200 OK\r\n"));
        EXPECT_TRUE(absolute->ends_with("\r\n\r\n/x?y"));
    }

    TEST(http_client_server, client_drops_a_pooled_connection_the_server_closed) {
        const test_server server{
            [](http::request& request) -> http::response {
                return {.body = std::to_string(request.peer().port)};
            },
            {.head_timeout = 200ms},
        };
        http::client client;

        // A POST is never sent twice, so the closed connection has to be noticed before it is used.
        const auto first = client.send({.method = "POST", .url = server.url("/")});
        ASSERT_TRUE(first) << first.error();

        std::this_thread::sleep_for(500ms);

        const auto second = client.send({.method = "POST", .url = server.url("/")});
        ASSERT_TRUE(second) << second.error();
        EXPECT_NE(first->body.text(), second->body.text());
    }

    TEST(http_client_server, client_drops_unread_input_and_retries_a_connection_closed_mid_request) {
        auto listener = listen_locally();
        ASSERT_TRUE(listener) << listener.error();
        const auto port = listener->local_endpoint().value().port;

        // Every connection answers one request. The first adds a response nobody asked for, and the
        // second closes once the next request has arrived.
        std::jthread server{[&] -> void {
            for (const std::string_view extra :
                 {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nextra", "", ""}) {
                auto socket = listener->accept();
                if (!socket || !socket->set_timeout(5s) || !receive_text(*socket, "\r\n\r\n")) {
                    return;
                }

                (void)send_text(*socket,
                                std::format("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok{}", extra));
                (void)receive_text(*socket, "\r\n\r\n");
            }
        }};

        http::client client;
        for (auto i = 0; i < 3; i++) {
            const auto response = client.send({.url = std::format("http://127.0.0.1:{}/", port)});
            ASSERT_TRUE(response) << response.error();
            EXPECT_EQ(response->body.text(), "ok");
        }
    }

    TEST(http_client_server, client_closes_idle_connections_to_origins_it_stopped_using) {
        auto listener = listen_locally();
        ASSERT_TRUE(listener) << listener.error();
        const auto port = listener->local_endpoint().value().port;

        // Answers one request, then waits for the client to close the connection.
        auto closed = false;
        std::jthread unused{[&] -> void {
            auto socket = listener->accept();
            if (!socket || !socket->set_timeout(5s) || !receive_text(*socket, "\r\n\r\n")) {
                return;
            }

            (void)send_text(*socket, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
            closed = receive_text(*socket).has_value();
        }};

        const test_server server{[](http::request&) -> http::response { return {}; }};
        http::client client{{.idle_timeout = 100ms}};

        ASSERT_TRUE(client.send({.url = std::format("http://127.0.0.1:{}/", port)}));
        std::this_thread::sleep_for(300ms);
        ASSERT_TRUE(client.send({.url = server.url("/")}));

        unused.join();
        EXPECT_TRUE(closed);
    }

    TEST(http_client_server, redirects_are_followed_only_when_enabled) {
        http::router router;
        ASSERT_TRUE(router.route("GET /old", [](http::request&) -> http::response {
            return {.status = http::status::found, .headers = {{.name = "Location", .value = "/new"}}};
        }));
        ASSERT_TRUE(router.route("POST /form", [](http::request&) -> http::response {
            return {.status = http::status::see_other, .headers = {{.name = "Location", .value = "/new"}}};
        }));
        ASSERT_TRUE(router.route("GET /loop", [](http::request&) -> http::response {
            return {.status = http::status::found, .headers = {{.name = "Location", .value = "/loop"}}};
        }));
        ASSERT_TRUE(router.route("/new", [](http::request& request) -> http::response {
            return {.body = request.head().method};
        }));

        const test_server server{router};

        http::client plain;
        const auto unfollowed = plain.send({.url = server.url("/old")});
        ASSERT_TRUE(unfollowed) << unfollowed.error();
        EXPECT_EQ(unfollowed->status, http::status::found);
        EXPECT_EQ(unfollowed->headers.find("Location"), "/new");

        http::client following{{.max_redirects = 3}};
        const auto followed = following.send({.url = server.url("/old")});
        ASSERT_TRUE(followed) << followed.error();
        EXPECT_EQ(followed->body.text(), "GET");

        const auto rewritten = following.send({.method = "POST", .url = server.url("/form"), .body = "data"});
        ASSERT_TRUE(rewritten) << rewritten.error();
        EXPECT_EQ(rewritten->body.text(), "GET");

        EXPECT_TRUE(tests::has_error(following.send({.url = server.url("/loop")}),
                                     http::error_code::too_many_redirects));
    }

    TEST(http_client_server, redirects_to_another_origin_drop_credentials_and_host) {
        const test_server target{[](http::request& request) -> http::response {
            const auto field = [&](std::string_view name) -> std::string_view {
                return request.header(name).value_or("none");
            };

            const auto seen =
                std::format("{} {} {}", field("authorization"), field("user-agent"), field("host"));
            return {.body = seen};
        }};
        const test_server origin{[&](http::request&) -> http::response {
            return {
                .status = http::status::found,
                .headers = {{.name = "Location", .value = target.url("/")}},
            };
        }};

        http::client client{{
            .max_redirects = 1,
            .headers = {{.name = "Authorization", .value = "secret"}, {.name = "User-Agent", .value = "rei"}},
        }};

        const auto response =
            client.send({.url = origin.url("/"), .headers = {{.name = "Host", .value = "virtual"}}});
        ASSERT_TRUE(response) << response.error();
        EXPECT_EQ(response->body.text(), std::format("none rei 127.0.0.1:{}", target.port()));
    }

    TEST(http_client_server, client_reports_timeouts_and_unsupported_urls) {
        const test_server server{[](http::request&) -> http::response {
            std::this_thread::sleep_for(300ms);
            return {};
        }};

        http::client impatient{{.io_timeout = 50ms}};
        EXPECT_TRUE(tests::has_error(impatient.send({.url = server.url("/")}), net::error_code::timed_out));

        http::client client;
        EXPECT_TRUE(tests::has_error(client.send({.url = "https://example.com/"}), std::errc::not_supported));
        EXPECT_TRUE(tests::has_error(client.send({.url = "http://user:secret@127.0.0.1/"}),
                                     std::errc::invalid_argument));
    }

    TEST(http_client_server, the_longest_timeouts_stand_for_none) {
        constexpr auto longest = std::chrono::milliseconds::max();

        const test_server server{
            [](http::request&) -> http::response { return {.body = "ok"}; },
            {.head_timeout = longest, .io_timeout = longest},
        };
        http::client client{{
            .connect_timeout = longest,
            .io_timeout = longest,
            .timeout = longest,
            .idle_timeout = longest,
        }};

        // The second request takes the connection of the first from the pool.
        for (auto i = 0; i < 2; i++) {
            const auto response = client.send({.url = server.url("/")});
            ASSERT_TRUE(response) << response.error();
            EXPECT_EQ(response->body.text(), "ok");
        }
    }

    TEST(http_client_server, stop_closes_idle_connections_and_lets_a_running_request_announce_it) {
        http::server* stopped = nullptr;
        http::server server{[&](http::request&) -> http::response {
            stopped->stop();
            return {.body = "ok"};
        }};
        stopped = &server;

        const auto local = server.listen({.address = "127.0.0.1", .port = 0});
        ASSERT_TRUE(local) << local.error();
        std::jthread running{[&] -> void { (void)server.run(); }};

        // Idles until the stop, which would otherwise wait for its head_timeout.
        const auto idle = connect_to(local->port);
        ASSERT_TRUE(idle) << idle.error();

        const auto start = std::chrono::steady_clock::now();
        const auto response = exchange_raw(local->port, "GET / HTTP/1.1\r\nHost: a\r\n\r\n");
        ASSERT_TRUE(response) << response.error();
        EXPECT_TRUE(response->contains("Connection: close\r\n"));
        EXPECT_TRUE(response->ends_with("\r\n\r\nok"));

        running.join();
        EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    }
} // namespace rei::net::tests
