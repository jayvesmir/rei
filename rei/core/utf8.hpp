#pragma once

#include <string>
#include <string_view>

namespace rei::core::utf8 {
    [[nodiscard]] auto is_valid(std::string_view text) noexcept -> bool;
    [[nodiscard]] auto valid_prefix(std::string_view text) noexcept -> std::string_view;
    [[nodiscard]] auto append(std::string& out, char32_t code_point) -> bool;
} // namespace rei::core::utf8
