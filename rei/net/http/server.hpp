#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/connection.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/socket.hpp"

namespace rei::net::http {
    // A request as a handler sees it. The views point into the connection's buffer and are valid for
    // the handler's call.
    class request {
      private:
        friend class server;
        friend class router;

        http::request_head m_head;
        std::string_view m_path;
        std::optional<std::string_view> m_query;
        std::vector<std::pair<std::string_view, std::string_view>> m_params;
        std::string m_decoded;
        net::ip_endpoint m_peer;

        http::connection* m_connection{};
        std::uint64_t m_max_body_size{};
        std::optional<std::uint64_t> m_body_size;
        std::uint64_t m_body_received{};
        std::vector<std::byte> m_body;
        bool m_body_buffered{};
        bool m_continue_pending{};

        request() = default;

        auto _start() -> std::optional<http::status>;
        auto _drain() -> bool;

      public:
        request(const request&) = delete;
        auto operator=(const request&) -> request& = delete;

        [[nodiscard]] auto head() const noexcept -> const http::request_head& { return m_head; }
        [[nodiscard]] auto path() const noexcept -> std::string_view { return m_path; }
        [[nodiscard]] auto query() const noexcept -> std::optional<std::string_view> { return m_query; }
        [[nodiscard]] auto header(std::string_view name) const noexcept -> std::optional<std::string_view> {
            return http::find_header(m_head.headers, name);
        }

        // Percent-decoded. Empty when the matched route has no wildcard called `name`.
        [[nodiscard]] auto param(std::string_view name) const noexcept -> std::string_view;
        [[nodiscard]] auto peer() const noexcept -> const net::ip_endpoint& { return m_peer; }

        // Answers `Expect: 100-continue` first. Returns 0 once the body is complete.
        auto read_body(std::span<std::byte> out) -> std::expected<std::size_t, rei::error>;

        // Both read the rest of the body, and fail with http::error_code::body_too_large beyond
        // server_options::max_body_size.
        auto body() -> std::expected<std::span<const std::byte>, rei::error>;
        auto text() -> std::expected<std::string_view, rei::error>;
    };

    using handler = std::function<auto(http::request&)->std::expected<http::response, rei::error>>;

    namespace detail {
        enum class segment_kind : std::uint8_t {
            literal,
            param,
            rest,
        };

        struct route_segment {
            detail::segment_kind kind{};
            std::string text; // the literal, or the wildcard's name
        };

        struct route {
            std::string method;
            std::vector<detail::route_segment> segments;
            http::handler handler;
        };
    } // namespace detail

    class router {
      private:
        std::vector<detail::route> m_routes;

      public:
        // "[METHOD ]/path", where {name} matches one segment and a final {name...} the rest. Among the
        // routes that match a request, the first literal segment from the left wins.
        auto route(std::string_view pattern, http::handler handler) -> std::expected<void, rei::error>;

        auto operator()(http::request& request) const -> std::expected<http::response, rei::error>;
    };

    struct server_options {
        std::size_t max_head_size{128uz * 1024};
        std::size_t max_headers{256};
        std::uint64_t max_body_size{1024uz * 1024 * 1024};
        std::chrono::milliseconds head_timeout{std::chrono::seconds{30}};
        std::chrono::milliseconds io_timeout{std::chrono::seconds{60}};
        std::optional<std::size_t> max_connections;

        // Called from connection threads, possibly at the same time.
        std::function<auto(const rei::error&)->void> on_error;
    };

    class server {
      private:
        struct session;

        http::handler m_handler;
        http::server_options m_options;
        net::socket m_listener;
        std::atomic<bool> m_stopping{false};
        std::mutex m_mutex;
        std::condition_variable m_changed;
        std::vector<server::session*> m_sessions;

        auto _serve(server::session& session) -> void;
        auto _spawn(net::socket socket) -> void;
        auto _register(server::session* session) -> void;
        auto _unregister(server::session* session) -> void;
        [[nodiscard]] auto _limits() const -> h1::parse_limits;
        auto _report(const rei::error& error) const -> void;

      public:
        explicit server(http::handler handler, http::server_options options = {});

        // Returns the bound endpoint, so port 0 works.
        auto listen(const net::ip_endpoint& local) -> std::expected<net::ip_endpoint, rei::error>;

        // Serves each connection on its own thread until stop(), then waits for them to finish.
        auto run() -> std::expected<void, rei::error>;

        // Only sets a flag, so a signal handler may call it. run() then closes the idle connections.
        auto stop() noexcept -> void { m_stopping = true; }

        // Serves one connection on the calling thread.
        auto serve(net::socket socket) -> void;
    };
} // namespace rei::net::http
