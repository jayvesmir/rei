#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "rei/core/error.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::backend {
    using native_handle = int;

    auto resolve(const net::ip_endpoint& endpoint, net::communication_domain domain,
                 net::transport_protocol protocol) -> std::expected<net::ip_endpoint, rei::error>;

    class socket_backend {
      private:
        backend::native_handle m_socket{-1};
        net::communication_domain m_domain{net::communication_domain::ipv4};
        net::transport_protocol m_protocol{net::transport_protocol::tcp};

        socket_backend(net::communication_domain domain, net::transport_protocol protocol) noexcept;
        auto _configure() -> std::expected<void, rei::error>;

      public:
        socket_backend() = default;
        ~socket_backend();

        socket_backend(const socket_backend&) = delete;
        auto operator=(const socket_backend&) -> socket_backend& = delete;
        socket_backend(socket_backend&& other) noexcept;
        auto operator=(socket_backend&& other) noexcept -> socket_backend&;

        static auto make(net::communication_domain domain, net::transport_protocol protocol)
            -> std::expected<socket_backend, rei::error>;

        [[nodiscard]] auto is_open() const noexcept -> bool { return m_socket != -1; }
        [[nodiscard]] auto native_handle() const noexcept -> native_handle { return m_socket; }
        [[nodiscard]] auto domain() const noexcept -> net::communication_domain { return m_domain; }
        [[nodiscard]] auto protocol() const noexcept -> net::transport_protocol { return m_protocol; }

        auto connect(const net::ip_endpoint& endpoint) -> std::expected<void, rei::error>;

        auto shutdown(net::shutdown_mode mode) -> std::expected<void, rei::error>;
        auto close() -> std::expected<void, rei::error>;

        auto bind(const net::ip_endpoint& local) -> std::expected<void, rei::error>;
        auto listen(std::int32_t backlog) -> std::expected<void, rei::error>;
        auto accept() -> std::expected<backend::socket_backend, rei::error>;

        auto send(std::span<const std::byte> data) -> std::expected<std::size_t, rei::error>;
        auto receive(std::span<std::byte> buffer) -> std::expected<std::size_t, rei::error>;
        auto send_to(std::span<const std::byte> data, const net::ip_endpoint& remote)
            -> std::expected<std::size_t, rei::error>;
        auto receive_from(std::span<std::byte> buffer) -> std::expected<net::datagram_result, rei::error>;

        [[nodiscard]] auto local_endpoint() const -> std::expected<net::ip_endpoint, rei::error>;
        [[nodiscard]] auto peer_endpoint() const -> std::expected<net::ip_endpoint, rei::error>;

        auto set_non_blocking(bool enabled) -> std::expected<void, rei::error>;
        auto set_reuse_address(bool enabled) -> std::expected<void, rei::error>;
    };
} // namespace rei::net::backend
