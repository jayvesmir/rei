#pragma once

#include <algorithm>
#include <string_view>

namespace rei::core {
    constexpr auto to_ascii_lower(char unit) noexcept -> char {
        return unit >= 'A' && unit <= 'Z' ? static_cast<char>(unit + ('a' - 'A')) : unit;
    }

    constexpr auto ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept -> bool {
        return std::ranges::equal(lhs, rhs, {}, to_ascii_lower, to_ascii_lower);
    }
} // namespace rei::core
