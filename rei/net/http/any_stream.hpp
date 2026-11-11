#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "rei/core/error.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/stream.hpp"
#include "rei/net/tls.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::http::detail {
    class any_stream {
      private:
        std::variant<net::socket, tls::stream> m_stream;

      public:
        explicit any_stream(net::socket socket) noexcept : m_stream(std::move(socket)) {}
        explicit any_stream(tls::stream stream) noexcept : m_stream(std::move(stream)) {}

        auto send(std::span<const std::byte> data) -> std::expected<std::size_t, rei::error> {
            return std::visit(
                [&](auto& stream) -> std::expected<std::size_t, rei::error> { return stream.send(data); },
                m_stream);
        }

        auto receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error> {
            return std::visit(
                [&](auto& stream) -> std::expected<std::size_t, rei::error> {
                    return stream.receive(buffer);
                },
                m_stream);
        }

        [[nodiscard]] auto readable() const noexcept -> bool {
            return std::visit([](const auto& stream) -> bool { return stream.readable(); }, m_stream);
        }

        auto shutdown(net::shutdown_mode mode = net::shutdown_mode::both) -> std::expected<void, rei::error> {
            return std::visit(
                [&](auto& stream) -> std::expected<void, rei::error> { return stream.shutdown(mode); },
                m_stream);
        }

        auto set_timeout(std::optional<std::chrono::milliseconds> timeout)
            -> std::expected<void, rei::error> {
            return std::visit(
                [&](auto& stream) -> std::expected<void, rei::error> { return stream.set_timeout(timeout); },
                m_stream);
        }

        auto set_deadline(std::optional<std::chrono::steady_clock::time_point> deadline) noexcept -> void {
            std::visit([&](auto& stream) -> void { stream.set_deadline(deadline); }, m_stream);
        }

        [[nodiscard]] auto socket() noexcept -> net::socket& {
            if (std::holds_alternative<tls::stream>(m_stream)) {
                return std::get<tls::stream>(m_stream).socket();
            }

            return std::get<net::socket>(m_stream);
        }
    };

    static_assert(net::stream<detail::any_stream>);
} // namespace rei::net::http::detail
