#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rei/core/utf8.hpp"

namespace rei::core::utf8 {
    auto is_valid(std::string_view text) noexcept -> bool {
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
                return false;
            }

            if (i + length > text.size()) {
                return false;
            }

            for (auto j = 1uz; j < length; j++) {
                auto byte = static_cast<std::uint8_t>(text[i + j]);
                if (byte < lower || byte > upper) {
                    return false;
                }

                lower = 0x80;
                upper = 0xBF;
            }

            i += length;
        }

        return true;
    }
} // namespace rei::core::utf8
