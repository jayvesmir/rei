#include <array>
#include <cstddef>
#include <string>
#include <system_error>

#include "gtest/gtest.h"

#include "rei/net/socket.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    TEST(socket_udp, datagrams_round_trip_with_their_origin) {
        auto server = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::udp);
        auto client = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::udp);
        ASSERT_TRUE(server && client);
        ASSERT_TRUE(server->bind(tests::loopback(net::communication_domain::ipv4)));
        auto server_address = server->local_endpoint();
        ASSERT_TRUE(server_address) << server_address.error();

        ASSERT_TRUE(client->connect(*server_address));
        EXPECT_EQ(client->peer_endpoint().value(), *server_address);
        auto sent = client->send(tests::as_bytes("ping"));
        ASSERT_TRUE(sent) << sent.error();
        EXPECT_EQ(*sent, 4U);

        std::array<std::byte, 16> buffer{};
        auto datagram = server->receive_from(buffer);
        ASSERT_TRUE(datagram) << datagram.error();
        EXPECT_EQ(datagram->size, 4U);
        EXPECT_EQ(datagram->origin, client->local_endpoint().value());
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), datagram->size), "ping");

        auto empty = server->send_to({}, datagram->origin);
        ASSERT_TRUE(empty) << empty.error();
        EXPECT_EQ(*empty, 0U);
        auto received = client->receive(buffer);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(*received, 0U);
    }

    TEST(socket_udp, oversized_datagram_is_rejected_as_message_size) {
        auto server = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::udp);
        auto client = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::udp);
        ASSERT_TRUE(server && client);
        ASSERT_TRUE(server->bind(tests::loopback(net::communication_domain::ipv4)));

        const std::string payload(64, 'x');
        ASSERT_TRUE(client->send_to(tests::as_bytes(payload), server->local_endpoint().value()));

        std::array<std::byte, 16> small{};
        EXPECT_TRUE(tests::has_error(server->receive_from(small), std::errc::message_size));
    }
} // namespace rei::net::tests
