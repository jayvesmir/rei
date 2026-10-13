#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rei::net {
    struct ip_endpoint {
        std::string address;
        std::uint16_t port{};

        auto operator==(const ip_endpoint&) const -> bool = default;
    };

    struct datagram_result {
        std::size_t size{};
        net::ip_endpoint origin;
    };
} // namespace rei::net
