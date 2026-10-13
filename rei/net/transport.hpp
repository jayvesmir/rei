#pragma once

#include <cstdint>

namespace rei::net {
    enum class transport_protocol : std::int32_t {
        tcp = 1,
        udp,
        sctp,
    };

    enum class communication_domain : std::int32_t {
        ipv4 = 1,
        ipv6,
    };

    enum class shutdown_mode : std::int32_t {
        receive,
        send,
        both,
    };
} // namespace rei::net
