#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/net/http.hpp"

// The HTTP/1.x wire format. Nothing here does I/O.

namespace rei::net::http::h1 {
    struct parse_limits {
        std::size_t max_head_size{16uz * 1024};
        std::size_t max_headers{100};
    };

    // The head's size in bytes, or std::nullopt until `input` holds a complete head. On success, the
    // views in `head` point into `input`.
    auto parse_head(std::span<const std::byte> input, http::request_head& head,
                    const h1::parse_limits& limits = {})
        -> std::expected<std::optional<std::size_t>, rei::error>;
    auto parse_head(std::span<const std::byte> input, http::response_head& head,
                    const h1::parse_limits& limits = {})
        -> std::expected<std::optional<std::size_t>, rei::error>;

    enum class body_kind : std::uint8_t {
        none,
        sized,
        chunked,
        until_close,
    };

    struct body_framing {
        h1::body_kind kind{h1::body_kind::none};
        std::uint64_t size{}; // only for body_kind::sized

        auto operator==(const body_framing&) const -> bool = default;
    };

    // RFC 9112 §6.3
    auto request_framing(const http::request_head& head) -> std::expected<h1::body_framing, rei::error>;
    auto response_framing(const http::response_head& head, std::string_view request_method)
        -> std::expected<h1::body_framing, rei::error>;

    auto keeps_alive(http::version version, std::span<const http::header> headers) noexcept -> bool;

    struct body_slice {
        std::size_t consumed{}; // framing included
        std::span<const std::byte> data;
    };

    class body_decoder {
      private:
        enum class state : std::uint8_t {
            sized,
            until_close,
            chunk_size,
            chunk_size_whitespace,
            chunk_extension,
            chunk_size_lf,
            chunk_data,
            chunk_data_cr,
            chunk_data_lf,
            trailer_start,
            trailer,
            trailer_lf,
            last_lf,
            done,
            failed,
        };

        std::uint64_t m_remaining{};
        std::uint32_t m_discarded{};
        std::uint8_t m_size_digits{};
        body_decoder::state m_state{state::done};

      public:
        explicit body_decoder(h1::body_framing framing) noexcept;

        // Returns at most one run of payload per call; call again with the unconsumed rest. Chunk
        // extensions and trailers are discarded, and more than 16 KB of them fails. Once decoding has
        // failed, all further input is rejected.
        auto decode(std::span<const std::byte> input) -> std::expected<h1::body_slice, rei::error>;

        [[nodiscard]] auto done() const noexcept -> bool { return m_state == state::done; }
    };

    auto serialize_head(const http::request_head& head, std::vector<std::byte>& out)
        -> std::expected<void, rei::error>;
    auto serialize_head(const http::response_head& head, std::vector<std::byte>& out)
        -> std::expected<void, rei::error>;

    auto write_chunk(std::span<const std::byte> data, std::vector<std::byte>& out) -> void;
    auto write_last_chunk(std::vector<std::byte>& out) -> void;
} // namespace rei::net::http::h1
