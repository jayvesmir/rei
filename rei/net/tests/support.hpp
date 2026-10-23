#pragma once

#include <cstddef>
#include <expected>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/net/endpoint.hpp"
#include "rei/net/socket.hpp"

namespace rei {
    // Lets GTest show which error a failed std::expected carried.
    inline auto operator<<(std::ostream& out, const rei::error& error) -> std::ostream& {
        return out << error.code().category().name() << ':' << error.code().value() << " \""
                   << error.code().message() << "\" " << error.detail();
    }
} // namespace rei

namespace rei::net {
    inline auto operator<<(std::ostream& out, const net::ip_endpoint& endpoint) -> std::ostream& {
        return out << '[' << endpoint.address << "]:" << endpoint.port;
    }
} // namespace rei::net

namespace rei::net::tests {
    struct tcp_pair {
        net::socket listener;
        net::socket client;
        net::socket accepted;
    };

    // Passes when `result` failed with `expected`, which is either a std::errc condition or a
    // net::error_code value.
    template <typename Value, typename Expected>
    auto has_error(const std::expected<Value, rei::error>& result, Expected expected)
        -> testing::AssertionResult {
        if (result) {
            return testing::AssertionFailure() << "operation succeeded";
        }
        if (result.error().code() != expected) {
            return testing::AssertionFailure() << "unexpected error " << result.error();
        }

        return testing::AssertionSuccess();
    }

    inline auto as_bytes(std::string_view text) -> std::span<const std::byte> {
        return std::as_bytes(std::span(text));
    }

    inline auto loopback(net::communication_domain domain) -> net::ip_endpoint {
        return {.address = domain == net::communication_domain::ipv4 ? "127.0.0.1" : "::1", .port = 0};
    }

    // Reads until `size` bytes arrived so tests don't depend on how TCP segments the stream.
    inline auto receive_exactly(net::socket& socket, std::size_t size)
        -> std::expected<std::string, rei::error> {
        std::string data(size, '\0');
        std::size_t received = 0;

        while (received < size) {
            auto chunk = socket.receive(std::as_writable_bytes(std::span(data)).subspan(received));
            if (!chunk) {
                return std::unexpected(chunk.error());
            }

            received += *chunk;
        }

        return data;
    }

    inline auto make_tcp_pair(net::communication_domain domain = net::communication_domain::ipv4)
        -> std::expected<tcp_pair, rei::error> {
        auto server = net::socket::make(domain, net::transport_protocol::tcp);
        auto client = net::socket::make(domain, net::transport_protocol::tcp);
        if (!server || !client) {
            return std::unexpected(server ? client.error() : server.error());
        }

        if (auto bound = server->bind(loopback(domain)); !bound) {
            return std::unexpected(bound.error());
        }

        if (auto listening = server->listen(); !listening) {
            return std::unexpected(listening.error());
        }

        auto local = server->local_endpoint();
        if (!local) {
            return std::unexpected(local.error());
        }

        if (auto connected = client->connect(*local); !connected) {
            return std::unexpected(connected.error());
        }

        auto accepted = server->accept();
        if (!accepted) {
            return std::unexpected(accepted.error());
        }

        return tests::tcp_pair{
            .listener = std::move(*server),
            .client = std::move(*client),
            .accepted = std::move(*accepted),
        };
    }
} // namespace rei::net::tests
