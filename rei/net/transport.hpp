#pragma once

namespace rei::net {
    enum class transport_protocol {
        tcp = 1,
        udp,
        sctp,
    };

    enum class communication_domain {
        ipv4 = 1,
        ipv6,
    };
} // namespace rei::net
