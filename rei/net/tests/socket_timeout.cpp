#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <system_error>

#include "gtest/gtest.h"

#include "rei/net/error_code.hpp"
#include "rei/net/socket.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    using namespace std::chrono_literals;

    TEST(socket_timeout, blocking_operations_time_out_after_the_configured_delay) {
        auto listener = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(listener) << listener.error();
        ASSERT_TRUE(listener->bind(loopback(net::communication_domain::ipv4)));
        ASSERT_TRUE(listener->listen());

        EXPECT_TRUE(has_error(listener->set_timeout(-1ms), std::errc::invalid_argument));
        EXPECT_TRUE(
            has_error(listener->set_timeout(std::chrono::milliseconds{std::numeric_limits<int>::max()} + 1ms),
                      std::errc::invalid_argument));
        ASSERT_TRUE(listener->set_timeout(50ms));
        EXPECT_EQ(listener->timeout(), 50ms);

        auto started = std::chrono::steady_clock::now();
        auto nobody = listener->accept();
        ASSERT_TRUE(has_error(nobody, net::error_code::timed_out));
        EXPECT_EQ(nobody.error().code(), std::errc::timed_out);
        EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);

        auto client = net::socket::make(net::communication_domain::ipv4, net::transport_protocol::tcp);
        ASSERT_TRUE(client) << client.error();
        ASSERT_TRUE(client->connect(listener->local_endpoint().value()));

        auto accepted = listener->accept();
        ASSERT_TRUE(accepted) << accepted.error();
        EXPECT_FALSE(accepted->timeout().has_value());

        ASSERT_TRUE(accepted->set_timeout(50ms));
        std::array<std::byte, 16> buffer{};
        started = std::chrono::steady_clock::now();
        EXPECT_TRUE(has_error(accepted->receive(buffer), net::error_code::timed_out));
        EXPECT_GE(std::chrono::steady_clock::now() - started, 50ms);

        ASSERT_TRUE(accepted->set_timeout(std::nullopt));
        EXPECT_FALSE(accepted->timeout().has_value());
    }

    TEST(socket_timeout, non_blocking_sockets_ignore_the_timeout) {
        auto pair = make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        ASSERT_TRUE(pair->accepted.set_timeout(5s));
        ASSERT_TRUE(pair->accepted.set_non_blocking(true));
        ASSERT_TRUE(pair->listener.set_timeout(5s));
        ASSERT_TRUE(pair->listener.set_non_blocking(true));

        std::array<std::byte, 16> buffer{};
        auto started = std::chrono::steady_clock::now();
        EXPECT_TRUE(has_error(pair->accepted.receive(buffer), std::errc::resource_unavailable_try_again));
        EXPECT_TRUE(has_error(pair->listener.accept(), std::errc::resource_unavailable_try_again));
        EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    }
} // namespace rei::net::tests
