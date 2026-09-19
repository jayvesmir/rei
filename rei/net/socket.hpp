#pragma once

#include <expected>
#include <new>

#include "backend/socket.hpp"
#include "rei/net/transport.hpp"

namespace rei::net {
    class socket {
      private:
        backend::socket_backend m_backend;

      public:
        template <net::communication_domain Domain, net::transport_protocol Protocol>
        static auto make() -> std::expected<net::socket, rei::error> {
            net::socket out{};
            new (&out.m_backend) backend::socket_backend(Domain, Protocol);

            auto res = out.m_backend.initialize_platform_socket();
            if (!res) {
                return std::unexpected(res.error());
            }

            return out;
        }
    };
} // namespace rei::net
