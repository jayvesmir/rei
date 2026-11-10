#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "rei/core/error.hpp"
#include "rei/net/http/detail.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"

namespace rei::net::http::h1 {
    namespace {
        constexpr auto max_chunk_size_digits = 16; // a std::uint64_t
        constexpr auto max_discarded_size = 16u * 1024;

        constexpr auto hex_value(char unit) -> int {
            if (unit >= '0' && unit <= '9') {
                return unit - '0';
            }
            if (unit >= 'a' && unit <= 'f') {
                return unit - 'a' + 10;
            }
            if (unit >= 'A' && unit <= 'F') {
                return unit - 'A' + 10;
            }

            return -1;
        }
    } // namespace

    body_decoder::body_decoder(h1::body_framing framing) noexcept {
        switch (framing.kind) {
        case body_kind::none:
            break;
        case body_kind::sized:
            m_remaining = framing.size;
            m_state = framing.size == 0 ? state::done : state::sized;
            break;
        case body_kind::chunked:
            m_state = state::chunk_size;
            break;
        case body_kind::until_close:
            m_state = state::until_close;
            break;
        }
    }

    auto body_decoder::decode(std::span<const std::byte> input) -> std::expected<h1::body_slice, rei::error> {
        auto pos = 0uz;
        while (pos < input.size()) {
            const auto unit = static_cast<char>(input[pos]);
            auto valid = true;
            auto discarded = false;

            switch (m_state) {
            case state::done:
                return h1::body_slice{.consumed = pos, .data = {}};

            case state::failed:
                valid = false;
                break;

            case state::until_close:
                return h1::body_slice{.consumed = input.size(), .data = input};

            case state::sized:
            case state::chunk_data: {
                const auto size =
                    static_cast<std::size_t>(std::min<std::uint64_t>(m_remaining, input.size() - pos));

                m_remaining -= size;
                if (m_remaining == 0) {
                    m_state = m_state == state::sized ? state::done : state::chunk_data_cr;
                }

                return h1::body_slice{.consumed = pos + size, .data = input.subspan(pos, size)};
            }

            case state::chunk_size:
                if (const auto digit = hex_value(unit); digit >= 0) {
                    valid = m_size_digits < max_chunk_size_digits;
                    m_remaining = (m_remaining << 4u) | static_cast<std::uint64_t>(digit);
                    m_size_digits++;
                } else if (m_size_digits == 0) {
                    valid = false;
                } else if (unit == '\r') {
                    m_state = state::chunk_size_lf;
                } else if (unit == ';') {
                    m_state = state::chunk_extension;
                    discarded = true;
                } else if (detail::is_whitespace(unit)) {
                    m_state = state::chunk_size_whitespace;
                    discarded = true;
                } else {
                    valid = false;
                }
                break;

            case state::chunk_size_whitespace:
                if (unit == '\r') {
                    m_state = state::chunk_size_lf;
                } else if (unit == ';') {
                    m_state = state::chunk_extension;
                    discarded = true;
                } else if (detail::is_whitespace(unit)) {
                    discarded = true;
                } else {
                    valid = false;
                }
                break;

            case state::chunk_extension:
                if (unit == '\r') {
                    m_state = state::chunk_size_lf;
                } else {
                    discarded = true;
                }
                break;

            case state::chunk_size_lf:
                valid = unit == '\n';
                m_state = m_remaining == 0 ? state::trailer_start : state::chunk_data;
                m_size_digits = 0;
                break;

            case state::chunk_data_cr:
                valid = unit == '\r';
                m_state = state::chunk_data_lf;
                break;

            case state::chunk_data_lf:
                valid = unit == '\n';
                m_state = state::chunk_size;
                break;

            case state::trailer_start:
            case state::trailer:
                if (unit == '\r') {
                    m_state = m_state == state::trailer_start ? state::last_lf : state::trailer_lf;
                } else {
                    m_state = state::trailer;
                    discarded = true;
                }
                break;

            case state::trailer_lf:
                valid = unit == '\n';
                m_state = state::trailer_start;
                break;

            case state::last_lf:
                valid = unit == '\n';
                m_state = state::done;
                break;
            }

            if (discarded) {
                m_discarded++;
                valid = valid && detail::matches(unit, detail::char_class::value) &&
                        m_discarded <= max_discarded_size;
            }

            if (!valid) {
                m_state = state::failed;
                return detail::failure(http::error_code::bad_chunk, "malformed chunked body");
            }

            pos++;
        }

        return h1::body_slice{.consumed = pos, .data = {}};
    }
} // namespace rei::net::http::h1
