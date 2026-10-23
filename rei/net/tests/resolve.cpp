#include <string>
#include <system_error>

#include "gtest/gtest.h"

#include "rei/net/socket.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    TEST(resolve, numeric_and_loopback_names_resolve_per_domain) {
        auto numeric4 = net::resolve({.address = "127.0.0.1", .port = 6767}, net::communication_domain::ipv4,
                                     net::transport_protocol::tcp);
        ASSERT_TRUE(numeric4) << numeric4.error();
        EXPECT_EQ(*numeric4, (net::ip_endpoint{.address = "127.0.0.1", .port = 6767}));

        auto numeric6 = net::resolve({.address = "::1", .port = 443}, net::communication_domain::ipv6,
                                     net::transport_protocol::udp);
        ASSERT_TRUE(numeric6) << numeric6.error();
        EXPECT_EQ(*numeric6, (net::ip_endpoint{.address = "::1", .port = 443}));

        // Only IPv4 is checked for localhost, since mapping it to ::1 depends on the host configuration.
        auto localhost4 = net::resolve({.address = "localhost", .port = 80}, net::communication_domain::ipv4,
                                       net::transport_protocol::tcp);
        ASSERT_TRUE(localhost4) << localhost4.error();
        EXPECT_EQ(*localhost4, (net::ip_endpoint{.address = "127.0.0.1", .port = 80}));

        EXPECT_FALSE(net::resolve({.address = "127.0.0.1", .port = 80}, net::communication_domain::ipv6,
                                  net::transport_protocol::tcp));
    }

    TEST(resolve, invalid_addresses_are_rejected) {
        EXPECT_TRUE(has_error(net::resolve({.address = "", .port = 80}, net::communication_domain::ipv4,
                                           net::transport_protocol::tcp),
                              std::errc::invalid_argument));
        EXPECT_TRUE(has_error(net::resolve({.address = std::string("127.0.0.1\0", 10), .port = 80},
                                           net::communication_domain::ipv4, net::transport_protocol::tcp),
                              std::errc::invalid_argument));

        auto socket = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(socket) << socket.error();
        EXPECT_TRUE(has_error(socket->connect({.address = "", .port = 80}), std::errc::invalid_argument));
    }
} // namespace rei::net::tests
