#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "rei/core/error.hpp"
#include "rei/core/url.hpp"

namespace rei::core::detail {
    inline constexpr std::int32_t eof = -1;

    constexpr auto is_ascii_digit(std::int32_t unit) -> bool {
        return unit >= '0' && unit <= '9';
    }
    constexpr auto is_ascii_alpha(std::int32_t unit) -> bool {
        return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z');
    }
    constexpr auto is_ascii_alphanumeric(std::int32_t unit) -> bool {
        return is_ascii_digit(unit) || is_ascii_alpha(unit);
    }
    constexpr auto to_ascii_lower(std::int32_t unit) -> char {
        return static_cast<char>(unit >= 'A' && unit <= 'Z' ? unit + ('a' - 'A') : unit);
    }

    enum class encode_set {
        c0_control,
        fragment,
        query,
        special_query,
        path,
        userinfo,
    };

    // url/encoding.cpp
    auto ascii_iequals(std::string_view lhs, std::string_view rhs) -> bool;
    auto append_percent_encoded(std::string& output, char unit) -> void;
    auto percent_encode(std::string& output, char unit, detail::encode_set set) -> void;
    auto percent_encode(std::string_view input, detail::encode_set set) -> std::string;
    auto percent_decode(std::string_view input) -> std::string;

    // url/host.cpp
    auto parse_host(std::string_view input, bool is_opaque) -> std::expected<std::string, rei::error>;

    // url/parser.cpp
    enum class parse_state {
        scheme_start,
        scheme,
        no_scheme,
        special_relative_or_authority,
        path_or_authority,
        relative,
        relative_slash,
        special_authority_slashes,
        special_authority_ignore_slashes,
        authority,
        host,
        port,
        file,
        file_slash,
        file_host,
        path_start,
        path,
        opaque_path,
        query,
        fragment,
    };

    auto is_special(std::string_view scheme) -> bool;
    auto default_port(std::string_view scheme) -> std::optional<std::uint16_t>;
    auto basic_parse(std::string_view raw_input, const detail::url_record_view* base, detail::url_record& url,
                     std::optional<detail::parse_state> state_override = std::nullopt)
        -> std::expected<void, rei::error>;

    template <typename String> auto includes_credentials(const detail::url_fields<String>& url) -> bool {
        return !url.username.empty() || !url.password.empty();
    }
} // namespace rei::core::detail
