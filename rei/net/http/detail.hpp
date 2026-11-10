#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include "rei/core/error.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/error_code.hpp"

namespace rei::net::http::detail {
    enum class char_class : std::uint8_t {
        token = 1u << 0u,
        value = 1u << 1u,
        target = 1u << 2u,
    };

    inline constexpr auto char_classes = [] -> std::array<std::uint8_t, 256> {
        constexpr std::string_view token_symbols = "!#$%&'*+-.^_`|~";

        std::array<std::uint8_t, 256> table{};
        for (auto byte = 0uz; byte < table.size(); byte++) {
            const auto is_digit = byte >= '0' && byte <= '9';
            const auto is_alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
            const auto is_visible = byte >= 0x21 && byte <= 0x7E;

            if (is_digit || is_alpha || token_symbols.contains(static_cast<char>(byte))) {
                table[byte] |= std::to_underlying(char_class::token);
            }
            if (is_visible || byte == ' ' || byte == '\t' || byte >= 0x80) {
                table[byte] |= std::to_underlying(char_class::value);
            }
            if (is_visible) {
                table[byte] |= std::to_underlying(char_class::target);
            }
        }

        return table;
    }();

    constexpr auto matches(char unit, detail::char_class set) -> bool {
        return (char_classes[static_cast<std::uint8_t>(unit)] & std::to_underlying(set)) != 0;
    }

    constexpr auto skip(std::string_view text, std::size_t position, detail::char_class set) -> std::size_t {
        while (position < text.size() && detail::matches(text[position], set)) {
            position++;
        }

        return position;
    }

    constexpr auto consists_of(std::string_view text, detail::char_class set) -> bool {
        return detail::skip(text, 0, set) == text.size();
    }

    constexpr auto is_whitespace(char unit) -> bool {
        return unit == ' ' || unit == '\t';
    }

    constexpr auto trim(std::string_view text) -> std::string_view {
        while (!text.empty() && detail::is_whitespace(text.front())) {
            text.remove_prefix(1);
        }
        while (!text.empty() && detail::is_whitespace(text.back())) {
            text.remove_suffix(1);
        }

        return text;
    }

    constexpr auto version_text(http::version version) -> std::string_view {
        switch (version) {
        case version::http_1_0:
            return "HTTP/1.0";
        case version::http_1_1:
            return "HTTP/1.1";
        }

        return {};
    }

    inline auto failure(http::error_code code, std::string detail,
                        std::source_location location = std::source_location::current())
        -> std::unexpected<rei::error> {
        return std::unexpected(rei::error{http::make_error_code(code), std::move(detail), location});
    }
} // namespace rei::net::http::detail
