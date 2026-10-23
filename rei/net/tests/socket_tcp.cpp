#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <system_error>
#include <thread>

#include "gtest/gtest.h"

#include "rei/net/error_code.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    using namespace std::chrono_literals;

    TEST(socket_tcp, accepted_pair_has_matching_endpoints_and_exchanges_data) {
        auto pair = tests::make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        EXPECT_EQ(pair->accepted.domain(), net::communication_domain::ipv4);
        EXPECT_EQ(pair->accepted.protocol(), net::transport_protocol::tcp);

        auto client_local = pair->client.local_endpoint();
        auto client_peer = pair->client.peer_endpoint();
        auto accepted_local = pair->accepted.local_endpoint();
        auto accepted_peer = pair->accepted.peer_endpoint();
        ASSERT_TRUE(client_local && client_peer && accepted_local && accepted_peer);
        EXPECT_EQ(*client_local, *accepted_peer);
        EXPECT_EQ(*client_peer, *accepted_local);
        EXPECT_EQ(*client_peer, pair->listener.local_endpoint().value());
        EXPECT_NE(client_local->port, 0);

        auto sent = pair->client.send(tests::as_bytes("hello"));
        ASSERT_TRUE(sent) << sent.error();
        EXPECT_EQ(*sent, 5U);

        auto received = tests::receive_exactly(pair->accepted, 5);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(*received, "hello");

        // An empty read must return 0 without blocking and without looking like a closed peer.
        auto empty = pair->accepted.receive({});
        ASSERT_TRUE(empty) << empty.error();
        EXPECT_EQ(*empty, 0U);
    }

    TEST(socket_tcp, ipv6_loopback_pair_exchanges_data) {
        auto pair = tests::make_tcp_pair(net::communication_domain::ipv6);
        if (!pair && (pair.error().code() == std::errc::address_family_not_supported ||
                      pair.error().code() == std::errc::address_not_available)) {
            GTEST_SKIP() << "host has no IPv6 loopback support: " << pair.error();
        }
        ASSERT_TRUE(pair) << pair.error();

        auto local = pair->accepted.local_endpoint();
        ASSERT_TRUE(local) << local.error();
        EXPECT_EQ(local->address, "::1");
        EXPECT_EQ(pair->accepted.domain(), net::communication_domain::ipv6);

        ASSERT_TRUE(pair->client.send(tests::as_bytes("hello")));
        auto received = tests::receive_exactly(pair->accepted, 5);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(*received, "hello");
    }

    TEST(socket_tcp, peer_close_reports_connection_closed) {
        auto pair = tests::make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        ASSERT_TRUE(pair->client.close());

        std::array<std::byte, 16> buffer{};
        auto received = pair->accepted.receive(buffer);
        ASSERT_TRUE(tests::has_error(received, net::error_code::connection_closed));
        EXPECT_EQ(received.error().code(), std::errc::not_connected);

        // The reset answering the first send may arrive after that send returned, so allow a few
        // attempts. Without MSG_NOSIGNAL this would kill the process instead of failing.
        std::expected<std::size_t, rei::error> sent;
        for (auto _ = 0; _ < 50 && (sent = pair->accepted.send(tests::as_bytes("late"))); _++) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_TRUE(tests::has_error(sent, net::error_code::connection_closed));
    }

    TEST(socket_tcp, shutdown_send_half_closes_without_closing_the_socket) {
        auto pair = tests::make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        ASSERT_TRUE(pair->client.shutdown(net::shutdown_mode::send));
        EXPECT_TRUE(pair->client.is_open());

        std::array<std::byte, 16> buffer{};
        EXPECT_TRUE(tests::has_error(pair->accepted.receive(buffer), net::error_code::connection_closed));

        ASSERT_TRUE(pair->accepted.send(tests::as_bytes("reply")));
        auto reply = tests::receive_exactly(pair->client, 5);
        ASSERT_TRUE(reply) << reply.error();
        EXPECT_EQ(*reply, "reply");

        EXPECT_TRUE(pair->client.close());
        EXPECT_FALSE(pair->client.is_open());
    }

    TEST(socket_tcp, connect_to_a_port_without_listener_is_refused) {
        // A bound socket that never listens keeps its port reserved and refuses connections.
        auto reserved = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(reserved) << reserved.error();
        ASSERT_TRUE(reserved->bind(loopback(net::communication_domain::ipv4)));
        auto target = reserved->local_endpoint();
        ASSERT_TRUE(target) << target.error();

        auto blocking = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(blocking) << blocking.error();
        EXPECT_TRUE(tests::has_error(blocking->connect(*target), std::errc::connection_refused));

        auto timed = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(timed) << timed.error();
        ASSERT_TRUE(timed->set_timeout(1s));
        EXPECT_TRUE(tests::has_error(timed->connect(*target), std::errc::connection_refused));

        // A timed connect to a live listener leaves a usable blocking connection behind.
        ASSERT_TRUE(reserved->listen());
        auto client = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(client) << client.error();
        ASSERT_TRUE(client->set_timeout(1s));
        ASSERT_TRUE(client->connect(*target));

        auto accepted = reserved->accept();
        ASSERT_TRUE(accepted) << accepted.error();
        ASSERT_TRUE(client->send(tests::as_bytes("ping")));

        auto received = tests::receive_exactly(*accepted, 4);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(*received, "ping");

        // Non-blocking sockets ignore timeouts, so timing out here shows connect restored blocking mode.
        std::array<std::byte, 4> buffer{};
        ASSERT_TRUE(client->set_timeout(20ms));
        EXPECT_TRUE(has_error(client->receive(buffer), net::error_code::timed_out));
    }
} // namespace rei::net::tests
