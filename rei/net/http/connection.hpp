#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/socket.hpp"

namespace rei::net::http {
    // Runs the HTTP/1 loops over a socket. It adds no fields and makes no keep-alive decisions.
    class connection {
      private:
        net::socket m_socket;
        h1::parse_limits m_limits;

        std::vector<std::byte> m_input;
        std::size_t m_body_start{};
        std::size_t m_begin{};
        std::size_t m_end{};
        h1::body_decoder m_decoder{{}};
        h1::body_kind m_body_kind{h1::body_kind::none};
        std::uint64_t m_body_left{};
        bool m_body_pending{};
        bool m_input_keeps_alive{true};

        std::vector<std::byte> m_output;
        h1::body_framing m_output_framing;
        std::uint64_t m_written{};
        bool m_writing{};
        bool m_output_keeps_alive{true};

        bool m_read_failed{};
        bool m_write_failed{};
        std::optional<std::chrono::steady_clock::time_point> m_deadline;
        std::optional<std::chrono::milliseconds> m_timeout;

        template <typename Head, typename Framing>
        auto _read_head(Head& head, Framing framing) -> std::expected<void, rei::error>;
        template <typename Head, typename Framing>
        auto _write_head(const Head& head, Framing framing) -> std::expected<void, rei::error>;

        auto _take(std::span<const std::byte> data) -> std::size_t;
        auto _end_of_input(const rei::error& error) -> std::expected<std::size_t, rei::error>;
        auto _arm() -> std::expected<void, rei::error>;
        auto _receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error>;
        auto _send(std::span<const std::byte> data) -> std::expected<void, rei::error>;
        auto _flush() -> std::expected<void, rei::error>;

      public:
        explicit connection(net::socket socket, const h1::parse_limits& limits = {}) noexcept;

        [[nodiscard]] auto socket() noexcept -> net::socket& { return m_socket; }

        // Once `deadline` has passed, reads and writes fail with net::error_code::timed_out.
        auto deadline(std::optional<std::chrono::steady_clock::time_point> deadline) noexcept -> void {
            m_deadline = deadline;
        }

        // How long a single read or write may wait for the peer.
        auto timeout(std::optional<std::chrono::milliseconds> timeout) noexcept -> void {
            m_timeout = timeout;
        }

        // The views in `head` stay valid until the next read_head. A peer that closes before sending
        // anything gives net::error_code::connection_closed.
        auto read_head(http::request_head& head) -> std::expected<void, rei::error>;
        auto read_head(http::response_head& head, std::string_view request_method)
            -> std::expected<void, rei::error>;

        // Returns 0 once the body is complete.
        auto read_body(std::span<std::byte> out) -> std::expected<std::size_t, rei::error>;

        // The head is sent with the first write_body or with finish. Its own fields decide the framing,
        // which write_body and finish enforce.
        auto write_head(const http::request_head& head) -> std::expected<void, rei::error>;
        auto write_head(const http::response_head& head, std::string_view request_method)
            -> std::expected<void, rei::error>;
        auto write_body(std::span<const std::byte> data) -> std::expected<void, rei::error>;
        auto finish() -> std::expected<void, rei::error>;

        // The last message finished in both directions and neither side asked to close.
        [[nodiscard]] auto reusable() const noexcept -> bool {
            return !m_read_failed && !m_write_failed && !m_body_pending && !m_writing &&
                   m_input_keeps_alive && m_output_keeps_alive;
        }
    };
} // namespace rei::net::http
