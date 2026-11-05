#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "rei/core/url.hpp"
#include "rei/core/url/detail.hpp"

namespace rei::core::detail {
    namespace {
        constexpr auto in_encode_set(char unit, detail::encode_set set) -> bool {
            auto byte = static_cast<std::uint8_t>(unit);
            if (byte < 0x20 || byte > 0x7E) {
                return true;
            }

            switch (set) {
            case encode_set::c0_control:
                return false;
            case encode_set::fragment:
                return std::string_view{" \"<>`"}.contains(unit);
            case encode_set::query:
                return std::string_view{" \"#<>"}.contains(unit);
            case encode_set::special_query:
                return std::string_view{" \"#<>'"}.contains(unit);
            case encode_set::path:
                return std::string_view{" \"#<>?^`{}"}.contains(unit);
            case encode_set::userinfo:
                return std::string_view{" \"#<>?^`{}/:;=@[\\]|"}.contains(unit);
            }

            std::unreachable();
        }

        // Bit `set` of a byte's entry tells whether that set encodes the byte, so encoding doesn't have to
        // search the set for every code unit.
        constexpr auto encode_set_table = [] -> std::array<std::uint8_t, 256> {
            std::array<std::uint8_t, 256> table{};
            for (auto set : {
                     encode_set::c0_control,
                     encode_set::fragment,
                     encode_set::query,
                     encode_set::special_query,
                     encode_set::path,
                     encode_set::userinfo,
                 }) {
                for (auto byte = 0uz; byte < table.size(); byte++) {
                    if (in_encode_set(static_cast<char>(byte), set)) {
                        table[byte] |= 1U << std::to_underlying(set);
                    }
                }
            }

            return table;
        }();
    } // namespace

    auto ascii_iequals(std::string_view lhs, std::string_view rhs) -> bool {
        return std::ranges::equal(lhs, rhs, {}, to_ascii_lower, to_ascii_lower);
    }

    auto append_percent_encoded(std::string& output, char unit) -> void {
        constexpr std::string_view hex = "0123456789ABCDEF";
        auto byte = static_cast<std::uint8_t>(unit);

        output += '%';
        output += hex[byte >> 4];
        output += hex[byte & 0x0F];
    }

    auto percent_encode(std::string& output, char unit, detail::encode_set set) -> void {
        if (((encode_set_table[static_cast<std::uint8_t>(unit)] >> std::to_underlying(set)) & 1U) != 0) {
            detail::append_percent_encoded(output, unit);
        } else {
            output += unit;
        }
    }

    auto percent_encode(std::string_view input, detail::encode_set set) -> std::string {
        std::string output;
        for (char unit : input) {
            detail::percent_encode(output, unit, set);
        }

        return output;
    }

    auto percent_decode(std::string_view input) -> std::string {
        if (!input.contains('%')) {
            return std::string{input};
        }

        std::string output;
        for (auto i = 0uz; i < input.size(); i++) {
            auto hex = input.substr(i + 1, 2);

            std::uint8_t byte{};
            if (input[i] == '%' && hex.size() == 2 &&
                std::from_chars(hex.data(), hex.data() + hex.size(), byte, 16).ptr ==
                    hex.data() + hex.size()) {
                output += static_cast<char>(byte);
                i += 2;
            } else {
                output += input[i];
            }
        }

        return output;
    }
} // namespace rei::core::detail
