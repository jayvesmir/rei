#pragma once

namespace rei::formats::toml::detail {
    constexpr auto is_bare(char unit) noexcept -> bool {
        return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') ||
               unit == '_' || unit == '-';
    }
} // namespace rei::formats::toml::detail
