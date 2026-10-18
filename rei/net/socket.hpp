#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <utility>

#include "rei/net/backend/socket.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/transport.hpp"

namespace rei::net {
    class socket {
      private:
        backend::socket_backend m_backend;

        explicit socket(backend::socket_backend backend) noexcept : m_backend(std::move(backend)) {}

      public:
        socket() = default;
        socket(const socket&) = delete;
        auto operator=(const socket&) -> socket& = delete;
        socket(socket&&) noexcept = default;
        auto operator=(socket&&) noexcept -> socket& = default;

        static auto make(net::communication_domain domain, net::transport_protocol protocol)
            -> std::expected<net::socket, rei::error> {
            auto result = backend::socket_backend::make(domain, protocol);
            if (!result) {
                return std::unexpected(result.error());
            }

            return socket{std::move(*result)};
        }

        // For aliasing purposes
        template <net::communication_domain Domain, net::transport_protocol Protocol>
        static auto make() -> std::expected<net::socket, rei::error> {
            return socket::make(Domain, Protocol);
        }

        [[nodiscard]] auto is_open() const noexcept -> bool { return m_backend.is_open(); }

        [[nodiscard]] auto native_handle() const noexcept -> backend::native_handle {
            return m_backend.native_handle();
        }

        [[nodiscard]] auto domain() const noexcept -> net::communication_domain { return m_backend.domain(); }
        [[nodiscard]] auto protocol() const noexcept -> net::transport_protocol {
            return m_backend.protocol();
        }

        auto close() -> std::expected<void, rei::error> { return m_backend.close(); }

        auto connect(const net::ip_endpoint& remote) -> std::expected<void, rei::error> {
            return m_backend.connect(remote);
        }

        auto bind(const net::ip_endpoint& local) -> std::expected<void, rei::error> {
            return m_backend.bind(local);
        }

        auto listen(std::int32_t backlog = 128) -> std::expected<void, rei::error> {
            return m_backend.listen(backlog);
        }

        auto accept() -> std::expected<net::socket, rei::error> {
            auto result = m_backend.accept();
            if (!result) {
                return std::unexpected(result.error());
            }

            return socket{std::move(*result)};
        }

        auto send(std::span<const std::byte> data) -> std::expected<std::size_t, rei::error> {
            return m_backend.send(data);
        }

        auto receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error> {
            return m_backend.receive(buffer);
        }

        auto send_to(std::span<const std::byte> data, const net::ip_endpoint& remote)
            -> std::expected<std::size_t, rei::error> {
            return m_backend.send_to(data, remote);
        }

        auto receive_from(std::span<std::byte> buffer) -> std::expected<net::datagram_result, rei::error> {
            return m_backend.receive_from(buffer);
        }

        auto shutdown(net::shutdown_mode mode = net::shutdown_mode::both) -> std::expected<void, rei::error> {
            return m_backend.shutdown(mode);
        }

        auto local_endpoint() const -> std::expected<net::ip_endpoint, rei::error> {
            return m_backend.local_endpoint();
        }

        auto peer_endpoint() const -> std::expected<net::ip_endpoint, rei::error> {
            return m_backend.peer_endpoint();
        }

        auto set_non_blocking(bool enabled) -> std::expected<void, rei::error> {
            return m_backend.set_non_blocking(enabled);
        }

        auto set_reuse_address(bool enabled) -> std::expected<void, rei::error> {
            return m_backend.set_reuse_address(enabled);
        }
    };

    inline auto resolve(const net::ip_endpoint& endpoint, net::communication_domain domain,
                        net::transport_protocol protocol) -> std::expected<net::ip_endpoint, rei::error> {
        return backend::resolve(endpoint, domain, protocol);
    }
} // namespace rei::net
