#include <format>
#include <system_error>

#include <netinet/in.h>

#include "magic_enum/magic_enum.hpp"

#include "backend/socket.hpp"
#include "rei/core/error.hpp"

#define REI_POSIX_ARG(constant)                 \
    rei::net::backend::detail::posix_argument { \
        constant, #constant                     \
    }

namespace rei::net::backend {
    socket_backend::socket_backend(net::communication_domain domain, net::transport_protocol protocol)
        : m_domain(domain), m_protocol(protocol) {
        switch (m_domain) {
        case net::communication_domain::ipv4: {
            m_posix.domain = REI_POSIX_ARG(AF_INET);
        } break;
        case net::communication_domain::ipv6: {
            m_posix.domain = REI_POSIX_ARG(AF_INET6);
        } break;
        }

        switch (m_protocol) {
        case net::transport_protocol::tcp: {
            m_posix.type = REI_POSIX_ARG(SOCK_STREAM);
            m_posix.protocol = REI_POSIX_ARG(IPPROTO_TCP);
        } break;
        case net::transport_protocol::udp: {
            m_posix.type = REI_POSIX_ARG(SOCK_DGRAM);
            m_posix.protocol = REI_POSIX_ARG(IPPROTO_UDP);
        } break;
        case net::transport_protocol::sctp: {
            m_posix.type = REI_POSIX_ARG(SOCK_SEQPACKET);
            m_posix.protocol = REI_POSIX_ARG(IPPROTO_SCTP);
        } break;
        }
    }

    auto socket_backend::initialize_platform_socket() -> std::expected<void, rei::error> {
        auto res = ::socket(m_posix.domain.value, m_posix.type.value, m_posix.protocol.value);
        if (res < 0) {
            return std::unexpected(rei::error{
                std::error_code(errno, std::generic_category()),
                std::format("failed to create a POSIX socket ({}, {}):\n  `socket(domain={} [{}], "
                            "type={} [{}], protocol={} [{}])` returned {}",
                            magic_enum::enum_name(m_domain), magic_enum::enum_name(m_domain),
                            m_posix.domain.name, m_posix.domain.value, m_posix.type.name, m_posix.type.value,
                            m_posix.protocol.name, m_posix.protocol.value, res)});
        }

        m_socket = res;
        return {};
    }
} // namespace rei::net::backend
