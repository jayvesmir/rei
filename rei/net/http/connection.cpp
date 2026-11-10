#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/connection.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/socket.hpp"

namespace rei::net::http {
    namespace {
        constexpr auto body_window = 16uz * 1024;
        // Room for a head of up to 16 KB and the body window after it, so that few heads grow the buffer.
        constexpr auto initial_input_size = body_window + (16uz * 1024);
        constexpr auto coalesce_size = 16uz * 1024;

        auto misuse(std::string detail, std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              std::move(detail), location});
        }
    } // namespace

    connection::connection(net::socket socket, const h1::parse_limits& limits) noexcept
        : m_socket(std::move(socket)), m_limits(limits) {
        // A head and its body can go out in separate sends, and Nagle's algorithm would hold the second
        // one back until the peer's delayed ACK.
        (void)m_socket.set_no_delay(true);
    }

    auto connection::_arm() -> std::expected<void, rei::error> {
        auto wait = m_timeout;

        if (m_deadline) {
            const auto left =
                std::chrono::ceil<std::chrono::milliseconds>(*m_deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                return std::unexpected(
                    rei::error{net::make_error_code(net::error_code::timed_out), "deadline passed"});
            }

            wait = wait ? std::min(*wait, left) : left;
        }

        return m_socket.set_timeout(wait);
    }

    auto connection::_receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error> {
        return _arm().and_then(
            [&] -> std::expected<std::size_t, rei::error> { return m_socket.receive(buffer); });
    }

    auto connection::_send(std::span<const std::byte> data) -> std::expected<void, rei::error> {
        while (!data.empty()) {
            auto sent = _arm().and_then(
                [&] -> std::expected<std::size_t, rei::error> { return m_socket.send(data); });
            if (!sent) {
                m_write_failed = true;
                return std::unexpected(sent.error());
            }

            data = data.subspan(*sent);
        }

        return {};
    }

    auto connection::_flush() -> std::expected<void, rei::error> {
        auto sent = _send(m_output);
        m_output.clear();

        return sent;
    }

    template <typename Head, typename Framing>
    auto connection::_read_head(Head& head, Framing framing) -> std::expected<void, rei::error> {
        if (m_read_failed) {
            return misuse("the connection failed earlier");
        }
        if (m_body_pending) {
            return misuse("the previous body hasn't been read");
        }

        std::ranges::copy(std::span{m_input}.subspan(m_begin, m_end - m_begin), m_input.begin());
        m_end -= m_begin;
        m_begin = 0;

        // Saturates, so the largest max_head_size means no limit instead of wrapping around to a small one.
        const auto capacity = std::max(m_limits.max_head_size, m_limits.max_head_size + body_window);
        if (m_input.empty()) {
            m_input.resize(std::min(initial_input_size, capacity));
        }

        while (true) {
            const auto parsed = h1::parse_head(std::span{m_input}.first(m_end), head, m_limits);
            if (!parsed) {
                m_read_failed = true;
                return std::unexpected(parsed.error());
            }

            if (*parsed) {
                // Growing moves the bytes, so the head is parsed again from the new buffer.
                if (m_input.size() - **parsed < body_window && m_input.size() < capacity) {
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

            const auto received = _receive(std::span{m_input}.subspan(m_end));
            if (!received) {
                m_read_failed = true;
                return std::unexpected(received.error());
            }

            m_end += *received;
        }
    }

    auto connection::read_head(http::request_head& head) -> std::expected<void, rei::error> {
        return _read_head(head,
                          [](const http::request_head& head) -> std::expected<h1::body_framing, rei::error> {
                              return h1::request_framing(head);
                          });
    }

    auto connection::read_head(http::response_head& head, std::string_view request_method)
        -> std::expected<void, rei::error> {
        return _read_head(
            head,
            [request_method](const http::response_head& head) -> std::expected<h1::body_framing, rei::error> {
                return h1::response_framing(head, request_method);
            });
    }

    auto connection::_take(std::span<const std::byte> data) -> std::size_t {
        if (m_body_kind == h1::body_kind::sized) {
            m_body_left -= data.size();
        }

        m_body_pending = !m_decoder.done();
        return data.size();
    }

    auto connection::_end_of_input(const rei::error& error) -> std::expected<std::size_t, rei::error> {
        if (m_body_kind == h1::body_kind::until_close && error.code() == net::error_code::connection_closed) {
            m_body_pending = false;
            m_input_keeps_alive = false;
            return 0;
        }

        m_read_failed = true;
        return std::unexpected(error);
    }

    auto connection::read_body(std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
        if (m_read_failed) {
            return misuse("the connection failed earlier");
        }
        if (out.empty()) {
            return misuse("empty output buffer");
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

                const auto received = _receive(out);
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

            const auto received = _receive(std::span{m_input}.subspan(m_body_start));
            if (!received) {
                return _end_of_input(received.error());
            }

            m_end = m_body_start + *received;
        }

        return 0;
    }

    auto connection::stale() -> bool {
        if (m_begin < m_end) {
            return true;
        }

        std::byte unread{};
        const auto received = m_socket.set_timeout(std::chrono::milliseconds{0})
                                  .and_then([&] -> std::expected<std::size_t, rei::error> {
                                      return m_socket.receive({&unread, 1});
                                  });

        return received || received.error().code() != net::error_code::timed_out;
    }

    template <typename Head, typename Framing>
    auto connection::_write_head(const Head& head, Framing framing) -> std::expected<void, rei::error> {
        if (m_write_failed) {
            return misuse("the connection failed earlier");
        }
        if (m_writing) {
            return misuse("the previous message isn't finished");
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

    auto connection::write_head(const http::request_head& head) -> std::expected<void, rei::error> {
        return _write_head(head,
                           [](const http::request_head& head) -> std::expected<h1::body_framing, rei::error> {
                               return h1::request_framing(head);
                           });
    }

    auto connection::write_head(const http::response_head& head, std::string_view request_method)
        -> std::expected<void, rei::error> {
        return _write_head(
            head,
            [request_method](const http::response_head& head) -> std::expected<h1::body_framing, rei::error> {
                return h1::response_framing(head, request_method);
            });
    }

    auto connection::write_body(std::span<const std::byte> data) -> std::expected<void, rei::error> {
        if (!m_writing) {
            return misuse("no message is being written");
        }
        if (data.empty()) {
            return {};
        }

        switch (m_output_framing.kind) {
        case h1::body_kind::none:
            return misuse("the message has no body");
        case h1::body_kind::sized:
            if (data.size() > m_output_framing.size - m_written) {
                return misuse("the body is longer than its Content-Length");
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
        if (m_output.size() + data.size() <= coalesce_size) {
            m_output.insert(m_output.end(), data.begin(), data.end());
            return _flush();
        }

        return _flush().and_then([&] -> std::expected<void, rei::error> { return _send(data); });
    }

    auto connection::finish() -> std::expected<void, rei::error> {
        if (!m_writing) {
            return misuse("no message is being written");
        }
        if (m_output_framing.kind == h1::body_kind::sized && m_written != m_output_framing.size) {
            m_write_failed = true;
            return misuse("the body is shorter than its Content-Length");
        }

        if (m_output_framing.kind == h1::body_kind::chunked) {
            h1::write_last_chunk(m_output);
        }

        m_writing = false;
        return _flush();
    }
} // namespace rei::net::http
