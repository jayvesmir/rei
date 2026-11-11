#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>

#include "rei/core/error.hpp"
#include "rei/net/transport.hpp"

namespace rei::net {
    template <typename Stream>
    concept stream =
        std::movable<Stream> && requires(Stream& stream, const Stream& unchanged,
                                         std::span<const std::byte> data, std::span<std::byte> buffer) {
            { stream.send(data) } -> std::same_as<std::expected<std::size_t, rei::error>>;
            { stream.receive(buffer) } -> std::same_as<std::expected<std::size_t, rei::error>>;
            { unchanged.readable() } -> std::same_as<bool>;
            { stream.shutdown(net::shutdown_mode::send) } -> std::same_as<std::expected<void, rei::error>>;
            {
                stream.set_timeout(std::optional<std::chrono::milliseconds>{})
            } -> std::same_as<std::expected<void, rei::error>>;
            stream.set_deadline(std::optional<std::chrono::steady_clock::time_point>{});
        };
} // namespace rei::net
