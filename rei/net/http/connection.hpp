#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/stream.hpp"

namespace rei::net::http {
    namespace detail {
        inline constexpr auto body_window = 16uz * 1024;
        // Room for a head of up to 16 KB and the body window after it, so that few heads grow the buffer.
        inline constexpr auto initial_input_size = body_window + (16uz * 1024);
        inline constexpr auto coalesce_size = 16uz * 1024;

        auto misuse(std::string detail, std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error>;
    } // namespace detail

    // `timeout` from now, for connection::deadline. A sum past the clock's range stays at its last time
    // point.
    [[nodiscard]] inline auto deadline_after(std::chrono::milliseconds timeout) noexcept
        -> std::chrono::steady_clock::time_point {
        const auto now = std::chrono::steady_clock::now();
        const auto latest = std::chrono::steady_clock::time_point::max();

        return timeout < std::chrono::floor<std::chrono::milliseconds>(latest - now) ? now + timeout : latest;
    }

    // Runs the HTTP/1 loops over a stream. It adds no fields and makes no keep-alive decisions.
    template <net::stream Stream> class connection {
      private:
        Stream m_stream;
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

        template <typename Head, typename Framing>
        auto _read_head(Head& head, Framing framing) -> std::expected<void, rei::error>;
        template <typename Head, typename Framing>
        auto _write_head(const Head& head, Framing framing) -> std::expected<void, rei::error>;

        auto _take(std::span<const std::byte> data) -> std::size_t;
        auto _end_of_input(const rei::error& error) -> std::expected<std::size_t, rei::error>;
        auto _send(std::span<const std::byte> data) -> std::expected<void, rei::error>;
        auto _flush() -> std::expected<void, rei::error>;

      public:
        explicit connection(Stream stream, const h1::parse_limits& limits = {}) noexcept;

        [[nodiscard]] auto stream() noexcept -> Stream& { return m_stream; }

        // Once `deadline` has passed, reads and writes fail with net::error_code::timed_out.
        auto deadline(std::optional<std::chrono::steady_clock::time_point> deadline) noexcept -> void {
            m_stream.set_deadline(deadline);
        }

        // How long a single read or write may wait for the peer.
        auto timeout(std::optional<std::chrono::milliseconds> timeout) -> std::expected<void, rei::error> {
            return m_stream.set_timeout(timeout);
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

        // The peer closed the connection, or sent bytes that nothing has read. A client checks this
        // before its next request, whose response those bytes would otherwise pass for.
        [[nodiscard]] auto stale() const noexcept -> bool { return m_begin < m_end || m_stream.readable(); }
    };

    template <net::stream Stream>
    connection<Stream>::connection(Stream stream, const h1::parse_limits& limits) noexcept
        : m_stream(std::move(stream)), m_limits(limits) {}

    template <net::stream Stream>
    auto connection<Stream>::_send(std::span<const std::byte> data) -> std::expected<void, rei::error> {
        while (!data.empty()) {
            const auto sent = m_stream.send(data);
            if (!sent) {
                m_write_failed = true;
                return std::unexpected(sent.error());
            }

            data = data.subspan(*sent);
        }

        return {};
    }

    template <net::stream Stream> auto connection<Stream>::_flush() -> std::expected<void, rei::error> {
        auto sent = _send(m_output);
        m_output.clear();

        return sent;
    }

    template <net::stream Stream>
    template <typename Head, typename Framing>
    auto connection<Stream>::_read_head(Head& head, Framing framing) -> std::expected<void, rei::error> {
        if (m_read_failed) {
            return detail::misuse("the connection failed earlier");
        }
        if (m_body_pending) {
            return detail::misuse("the previous body hasn't been read");
        }

        std::ranges::copy(std::span{m_input}.subspan(m_begin, m_end - m_begin), m_input.begin());
        m_end -= m_begin;
        m_begin = 0;

        // Without a limit the buffer grows as far as the head does. The sum stays at the largest size
        // instead of wrapping around to a small one.
        const auto head_size = m_limits.max_head_size.value_or(std::numeric_limits<std::size_t>::max());
        const auto capacity = std::max(head_size, head_size + detail::body_window);
        if (m_input.empty()) {
            m_input.resize(std::min(detail::initial_input_size, capacity));
        }

        while (true) {
            const auto parsed = h1::parse_head(std::span{m_input}.first(m_end), head, m_limits);
            if (!parsed) {
                m_read_failed = true;
                return std::unexpected(parsed.error());
            }

            if (*parsed) {
                // Growing moves the bytes, so the head is parsed again from the new buffer.
                if (m_input.size() - **parsed < detail::body_window && m_input.size() < capacity) {
                    m_input.resize(std::min(m_input.size() * 2, capacity));
                    continue;
                }

                const auto body = framing(head);
                if (!body) {
                    m_read_failed = true;
                    return std::unexpected(body.error());
                }

                m_body_start = **parsed;
                m_begin = m_body_start;
                m_decoder = h1::body_decoder{*body};
                m_body_kind = body->kind;
                m_body_left = body->size;
                m_body_pending = !m_decoder.done();
                m_input_keeps_alive =
                    h1::keeps_alive(head.version, head.headers) && body->kind != h1::body_kind::until_close;

                return {};
            }

            if (m_end == m_input.size()) {
                m_input.resize(std::min(m_input.size() * 2, capacity));
            }

            const auto received = m_stream.receive(std::span{m_input}.subspan(m_end));
            if (!received) {
                m_read_failed = true;
                return std::unexpected(received.error());
            }

            m_end += *received;
        }
    }

    template <net::stream Stream>
    auto connection<Stream>::read_head(http::request_head& head) -> std::expected<void, rei::error> {
        return _read_head(head,
                          [](const http::request_head& head) -> std::expected<h1::body_framing, rei::error> {
                              return h1::request_framing(head);
                          });
    }

    template <net::stream Stream>
    auto connection<Stream>::read_head(http::response_head& head, std::string_view request_method)
        -> std::expected<void, rei::error> {
        return _read_head(
            head,
            [request_method](const http::response_head& head) -> std::expected<h1::body_framing, rei::error> {
                return h1::response_framing(head, request_method);
            });
    }

    template <net::stream Stream>
    auto connection<Stream>::_take(std::span<const std::byte> data) -> std::size_t {
        if (m_body_kind == h1::body_kind::sized) {
            m_body_left -= data.size();
        }

        m_body_pending = !m_decoder.done();
        return data.size();
    }

    template <net::stream Stream>
    auto connection<Stream>::_end_of_input(const rei::error& error)
        -> std::expected<std::size_t, rei::error> {
        if (m_body_kind == h1::body_kind::until_close && error.code() == net::error_code::connection_closed) {
            m_body_pending = false;
            m_input_keeps_alive = false;
            return 0;
        }

        m_read_failed = true;
        return std::unexpected(error);
    }

    template <net::stream Stream>
    auto connection<Stream>::read_body(std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
        if (m_read_failed) {
            return detail::misuse("the connection failed earlier");
        }
        if (out.empty()) {
            return detail::misuse("empty output buffer");
        }

        while (m_body_pending) {
            if (m_begin < m_end) {
                const auto input = std::span{m_input}.subspan(m_begin, std::min(m_end - m_begin, out.size()));

                const auto slice = m_decoder.decode(input);
                if (!slice) {
                    m_read_failed = true;
                    return std::unexpected(slice.error());
                }

                m_begin += slice->consumed;
                std::ranges::copy(slice->data, out.begin());
                if (!slice->data.empty() || m_decoder.done()) {
                    return _take(slice->data);
                }

                continue;
            }

            m_begin = m_body_start;
            m_end = m_body_start;

            const auto direct =
                (m_body_kind == h1::body_kind::sized || m_body_kind == h1::body_kind::until_close) &&
                out.size() >= m_input.size() - m_body_start;
            if (direct) {
                if (m_body_kind == h1::body_kind::sized) {
                    out =
                        out.first(static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), m_body_left)));
                }

                const auto received = m_stream.receive(out);
                if (!received) {
                    return _end_of_input(received.error());
                }

                const auto slice = m_decoder.decode(out.first(*received));
                if (!slice) {
                    m_read_failed = true;
                    return std::unexpected(slice.error());
                }

                return _take(slice->data);
            }

            const auto received = m_stream.receive(std::span{m_input}.subspan(m_body_start));
            if (!received) {
                return _end_of_input(received.error());
            }

            m_end = m_body_start + *received;
        }

        return 0;
    }

    template <net::stream Stream>
    template <typename Head, typename Framing>
    auto connection<Stream>::_write_head(const Head& head, Framing framing)
        -> std::expected<void, rei::error> {
        if (m_write_failed) {
            return detail::misuse("the connection failed earlier");
        }
        if (m_writing) {
            return detail::misuse("the previous message isn't finished");
        }

        const auto body = framing(head);
        if (!body) {
            return std::unexpected(body.error());
        }

        m_output.clear();
        if (auto serialized = h1::serialize_head(head, m_output); !serialized) {
            return serialized;
        }

        m_output_framing = *body;
        m_written = 0;
        m_writing = true;
        m_output_keeps_alive =
            h1::keeps_alive(head.version, head.headers) && body->kind != h1::body_kind::until_close;

        return {};
    }

    template <net::stream Stream>
    auto connection<Stream>::write_head(const http::request_head& head) -> std::expected<void, rei::error> {
        return _write_head(head,
                           [](const http::request_head& head) -> std::expected<h1::body_framing, rei::error> {
                               return h1::request_framing(head);
                           });
    }

    template <net::stream Stream>
    auto connection<Stream>::write_head(const http::response_head& head, std::string_view request_method)
        -> std::expected<void, rei::error> {
        return _write_head(
            head,
            [request_method](const http::response_head& head) -> std::expected<h1::body_framing, rei::error> {
                return h1::response_framing(head, request_method);
            });
    }

    template <net::stream Stream>
    auto connection<Stream>::write_body(std::span<const std::byte> data) -> std::expected<void, rei::error> {
        if (!m_writing) {
            return detail::misuse("no message is being written");
        }
        if (data.empty()) {
            return {};
        }

        switch (m_output_framing.kind) {
        case h1::body_kind::none:
            return detail::misuse("the message has no body");
        case h1::body_kind::sized:
            if (data.size() > m_output_framing.size - m_written) {
                return detail::misuse("the body is longer than its Content-Length");
            }
            break;
        case h1::body_kind::chunked:
            h1::write_chunk(data, m_output);
            m_written += data.size();
            return _flush();
        case h1::body_kind::until_close:
            break;
        }

        m_written += data.size();
        if (m_output.size() + data.size() <= detail::coalesce_size) {
            m_output.insert(m_output.end(), data.begin(), data.end());
            return _flush();
        }

        return _flush().and_then([&] -> std::expected<void, rei::error> { return _send(data); });
    }

    template <net::stream Stream> auto connection<Stream>::finish() -> std::expected<void, rei::error> {
        if (!m_writing) {
            return detail::misuse("no message is being written");
        }
        if (m_output_framing.kind == h1::body_kind::sized && m_written != m_output_framing.size) {
            m_write_failed = true;
            return detail::misuse("the body is shorter than its Content-Length");
        }

        if (m_output_framing.kind == h1::body_kind::chunked) {
            h1::write_last_chunk(m_output);
        }

        m_writing = false;
        return _flush();
    }
} // namespace rei::net::http
