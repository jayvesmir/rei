#pragma once

#include <string_view>

namespace rei::core::utf8 {
    [[nodiscard]] auto is_valid(std::string_view text) noexcept -> bool;
} // namespace rei::core::utf8
