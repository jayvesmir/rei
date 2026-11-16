#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "rei/core/utf8.hpp"

namespace rei::core::utf8 {
    auto is_valid(std::string_view text) noexcept -> bool {
        return valid_prefix(text).size() == text.size();
    }

    auto valid_prefix(std::string_view text) noexcept -> std::string_view {
        for (auto i = 0uz; i < text.size();) {
            auto lead = static_cast<std::uint8_t>(text[i]);
            if (lead < 0x80) {
                i++;
                continue;
            }

            // The second byte's range is narrowed for some leads to rule out overlong forms, surrogates
            // and code points above U+10FFFF.
            std::size_t length = 0;
            std::uint8_t lower = 0x80;
            std::uint8_t upper = 0xBF;
            if (lead >= 0xC2 && lead <= 0xDF) {
                length = 2;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                length = 3;
                lower = lead == 0xE0 ? 0xA0 : lower;
                upper = lead == 0xED ? 0x9F : upper;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                length = 4;
                lower = lead == 0xF0 ? 0x90 : lower;
                upper = lead == 0xF4 ? 0x8F : upper;
            } else {
                return text.substr(0, i);
            }

            if (i + length > text.size()) {
                return text.substr(0, i);
            }

            for (auto j = 1uz; j < length; j++) {
                auto byte = static_cast<std::uint8_t>(text[i + j]);
                if (byte < lower || byte > upper) {
                    return text.substr(0, i);
                }

                lower = 0x80;
                upper = 0xBF;
            }

            i += length;
        }

        return text;
    }

    auto append(std::string& out, char32_t code_point) -> bool {
        const auto scalar = static_cast<std::uint32_t>(code_point);
        if (scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
            return false;
        }

        const auto continuation = [&](std::uint32_t shift) -> char {
            return static_cast<char>(0x80 | ((scalar >> shift) & 0x3F));
        };

        if (scalar < 0x80) {
            out += static_cast<char>(scalar);
        } else if (scalar < 0x800) {
            out += static_cast<char>(0xC0 | (scalar >> 6));
            out += continuation(0);
        } else if (scalar < 0x10000) {
            out += static_cast<char>(0xE0 | (scalar >> 12));
            out += continuation(6);
            out += continuation(0);
        } else {
            out += static_cast<char>(0xF0 | (scalar >> 18));
            out += continuation(12);
            out += continuation(6);
            out += continuation(0);
        }

        return true;
    }
} // namespace rei::core::utf8
