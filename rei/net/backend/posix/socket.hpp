#pragma once

#include <expected>

#include <sys/socket.h>

#include "rei/core/error.hpp"
#include "rei/net/transport.hpp"

namespace rei::net::backend {
    using descriptor_t = int;

    namespace detail {
        struct posix_argument {
            int value{};
            std::string_view name{"unknown"};
        };
    } // namespace detail

    class socket_backend {
        descriptor_t m_socket{-1};

        net::communication_domain m_domain{net::communication_domain::ipv4};
        net::transport_protocol m_protocol{net::transport_protocol::tcp};

        struct {
            detail::posix_argument domain;
            detail::posix_argument type;
            detail::posix_argument protocol;
        } m_posix;

      public:
        socket_backend() = default;
        explicit socket_backend(net::communication_domain domain, net::transport_protocol protocol);

        auto initialize_platform_socket() -> std::expected<void, rei::error>;
    };
} // namespace rei::net::backend
