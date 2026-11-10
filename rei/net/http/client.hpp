#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "rei/core/error.hpp"
#include "rei/net/http.hpp"

namespace rei::net::http {
    namespace detail {
        struct pool;
    } // namespace detail

    struct client_options {
        // std::nullopt waits for as long as it takes.
        std::optional<std::chrono::milliseconds> connect_timeout{std::chrono::seconds{30}};
        std::optional<std::chrono::milliseconds> io_timeout{std::chrono::seconds{60}};
        // A deadline for the whole exchange. For stream(), it ends with the head.
        std::optional<std::chrono::milliseconds> timeout;
        std::uint32_t max_redirects{};
        std::size_t max_head_size{128uz * 1024};
        std::size_t max_headers{100};
        std::uint64_t max_body_size{64uz * 1024 * 1024};
        std::size_t max_idle_per_host{4};
        // std::nullopt keeps an idle connection until the server closes it.
        std::optional<std::chrono::milliseconds> idle_timeout{std::chrono::seconds{90}};

        // Added to every request that doesn't set the same name.
        http::headers headers;
    };

    struct client_request {
        std::string method{"GET"};
        std::string url;
        http::headers headers;
        http::body body;
    };

    // Copies share their connection pool. Safe to use from several threads at once.
    class client {
      private:
        http::client_options m_options;
        std::shared_ptr<detail::pool> m_pool;

        auto _open(http::client_request request,
                   std::optional<std::chrono::steady_clock::time_point> deadline, bool deadline_covers_body)
            -> std::expected<http::response, rei::error>;

      public:
        explicit client(http::client_options options = {});

        // Reads the whole body, up to max_body_size.
        auto send(http::client_request request) -> std::expected<http::response, rei::error>;

        // Returns after the head. The body streams from the connection, which goes back to the pool
        // once the body has been read to the end.
        auto stream(http::client_request request) -> std::expected<http::response, rei::error>;
    };
} // namespace rei::net::http
