#include <array>
#include <chrono>
#include <cstddef>
#include <system_error>
#include <type_traits>
#include <utility>

#include "gtest/gtest.h"

#include "rei/net/socket.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    using namespace std::chrono_literals;

    static_assert(!std::is_copy_constructible_v<net::socket> && !std::is_copy_assignable_v<net::socket>);
    static_assert(std::is_nothrow_move_constructible_v<net::socket> &&
                  std::is_nothrow_move_assignable_v<net::socket>);

    TEST(socket_lifecycle, make_reports_configuration_and_rejects_unsupported_protocols) {
        auto socket = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::udp);
        ASSERT_TRUE(socket) << socket.error();
        EXPECT_TRUE(socket->is_open());
        EXPECT_EQ(socket->domain(), net::communication_domain::ipv4);
        EXPECT_EQ(socket->protocol(), net::transport_protocol::udp);
        EXPECT_FALSE(socket->timeout().has_value());

        EXPECT_FALSE(net::socket{}.is_open());

        auto sctp = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::sctp);
        EXPECT_TRUE(tests::has_error(sctp, std::errc::protocol_not_supported));
    }

    TEST(socket_lifecycle, moves_transfer_ownership_and_assignment_releases_the_old_socket) {
        auto source = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(source) << source.error();
        ASSERT_TRUE(source->set_timeout(250ms));
        ASSERT_TRUE(source->bind(loopback(net::communication_domain::ipv4)));

        auto address = source->local_endpoint();
        ASSERT_TRUE(address) << address.error();

        const auto handle = source->native_handle();

        net::socket moved{std::move(*source)};
        EXPECT_FALSE(source->is_open());
        EXPECT_TRUE(moved.is_open());
        EXPECT_EQ(moved.native_handle(), handle);
        EXPECT_EQ(moved.protocol(), net::transport_protocol::tcp);
        EXPECT_EQ(moved.timeout(), 250ms);

        auto other = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(other) << other.error();
        EXPECT_TRUE(tests::has_error(other->bind(*address), std::errc::address_in_use));

        moved = std::move(*other);
        EXPECT_FALSE(other->is_open());

        auto rebound = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(rebound) << rebound.error();
        EXPECT_TRUE(rebound->bind(*address));
    }

    TEST(socket_lifecycle, closed_and_mismatched_sockets_reject_operations) {
        auto tcp = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        auto udp = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::udp);
        ASSERT_TRUE(tcp && udp);

        std::array<std::byte, 8> buffer{};
        const auto remote = tests::loopback(net::communication_domain::ipv4);

        EXPECT_TRUE(tests::has_error(udp->listen(), std::errc::operation_not_supported));
        EXPECT_TRUE(tests::has_error(udp->accept(), std::errc::operation_not_supported));
        EXPECT_TRUE(tests::has_error(tcp->send_to(buffer, remote), std::errc::operation_not_supported));
        EXPECT_TRUE(tests::has_error(tcp->receive_from(buffer), std::errc::operation_not_supported));

        ASSERT_TRUE(tcp->close());
        ASSERT_TRUE(udp->close());
        EXPECT_FALSE(tcp->is_open());
        EXPECT_TRUE(tcp->close());

        EXPECT_TRUE(tests::has_error(tcp->connect({.address = "127.0.0.1", .port = 1}),
                                     std::errc::bad_file_descriptor));
        EXPECT_TRUE(tests::has_error(tcp->bind(remote), std::errc::bad_file_descriptor));
        EXPECT_TRUE(tests::has_error(tcp->accept(), std::errc::bad_file_descriptor));
        EXPECT_TRUE(tests::has_error(tcp->send(buffer), std::errc::bad_file_descriptor));
        EXPECT_TRUE(tests::has_error(tcp->receive(buffer), std::errc::bad_file_descriptor));
        EXPECT_TRUE(tests::has_error(udp->send_to(buffer, remote), std::errc::bad_file_descriptor));
        EXPECT_TRUE(tests::has_error(udp->receive_from(buffer), std::errc::bad_file_descriptor));
    }
} // namespace rei::net::tests
