#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "rei/core/url.hpp"
#include "rei/core/url/detail.hpp"
#include "rei/core/utf8.hpp"

namespace rei::core::detail {
    namespace {
        auto is_windows_drive_letter(std::string_view text) -> bool {
            return text.size() == 2 && is_ascii_alpha(text[0]) && (text[1] == ':' || text[1] == '|');
        }

        auto starts_with_windows_drive_letter(std::string_view text) -> bool {
            return text.size() >= 2 && is_windows_drive_letter(text.substr(0, 2)) &&
                   (text.size() == 2 || std::string_view{"/\\?#"}.contains(text[2]));
        }

        // Windows (ꐦ¬_¬)
        auto first_segment_is_drive_letter(std::string_view path) -> bool {
            return path.size() >= 3 && path[0] == '/' && is_ascii_alpha(path[1]) && path[2] == ':' &&
                   (path.size() == 3 || path[3] == '/');
        }

        auto shorten_path(detail::url_record& url) -> void {
            if (url.scheme == "file" && url.path.size() == 3 &&
                detail::first_segment_is_drive_letter(url.path)) {
                return;
            }

            if (auto last = url.path.rfind('/'); last != std::string::npos) {
                url.path.erase(last);
            }
        }

        auto is_single_dot_segment(std::string_view segment) -> bool {
            return segment == "." || detail::ascii_iequals(segment, "%2e");
        }

        auto is_double_dot_segment(std::string_view segment) -> bool {
            return segment == ".." || detail::ascii_iequals(segment, ".%2e") ||
                   detail::ascii_iequals(segment, "%2e.") || detail::ascii_iequals(segment, "%2e%2e");
        }
    } // namespace

    auto is_special(std::string_view scheme) -> bool {
        using namespace std::string_view_literals;
        return std::ranges::contains(std::array{"ftp"sv, "file"sv, "http"sv, "https"sv, "ws"sv, "wss"sv},
                                     scheme);
    }

    auto default_port(std::string_view scheme) -> std::optional<std::uint16_t> {
        if (scheme == "http" || scheme == "ws") {
            return 80;
        }

        if (scheme == "https" || scheme == "wss") {
            return 443;
        }

        if (scheme == "ftp") {
            return 21;
        }

        return std::nullopt;
    }

    auto basic_parse(std::string_view raw_input, const detail::url_record_view* base, detail::url_record& url,
                     std::optional<detail::parse_state> state_override) -> std::expected<void, rei::error> {
        if (!utf8::is_valid(raw_input)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "input is not valid UTF-8"});
        }

        if (!state_override) {
            auto is_c0_control_or_space = [](char unit) -> bool {
                return static_cast<std::uint8_t>(unit) <= 0x20;
            };
            while (!raw_input.empty() && is_c0_control_or_space(raw_input.front())) {
                raw_input.remove_prefix(1);
            }
            while (!raw_input.empty() && is_c0_control_or_space(raw_input.back())) {
                raw_input.remove_suffix(1);
            }
        }

        auto input = std::ranges::to<std::string>(raw_input | std::views::filter([](char unit) -> bool {
                                                      return unit != '\t' && unit != '\n' && unit != '\r';
                                                  }));

        const auto size = std::ssize(input);
        auto state = state_override.value_or(parse_state::scheme_start);
        std::string buffer;
        bool at_sign_seen = false;
        bool inside_brackets = false;
        bool password_token_seen = false;
        std::ptrdiff_t pointer = 0;

        // Most states check this for every code point, so it's only recomputed when the scheme changes.
        bool special = detail::is_special(url.scheme);
        auto set_scheme = [&](std::string_view scheme) -> void {
            url.scheme = scheme;
            special = detail::is_special(scheme);
        };

        auto remaining = [&] -> std::string_view {
            return std::string_view{input}.substr(static_cast<std::size_t>(std::min(pointer + 1, size)));
        };
        auto copy_authority = [&] -> void {
            url.username = base->username;
            url.password = base->password;
            url.host = base->host;
            url.port = base->port;
        };

        for (;; pointer++) {
            const std::int32_t unit = pointer < size ? static_cast<std::uint8_t>(input[pointer]) : eof;

            switch (state) {
            case parse_state::scheme_start:
                if (is_ascii_alpha(unit)) {
                    buffer += to_ascii_lower(unit);
                    state = parse_state::scheme;
                } else if (!state_override) {
                    state = parse_state::no_scheme;
                    pointer--;
                } else {
                    return std::unexpected(rei::error{
                        core::make_error_code(core::error_code::invalid_argument), "invalid scheme"});
                }
                break;

            case parse_state::scheme:
                if (is_ascii_alphanumeric(unit) || unit == '+' || unit == '-' || unit == '.') {
                    buffer += to_ascii_lower(unit);
                } else if (unit == ':') {
                    if (state_override) {
                        if (special != detail::is_special(buffer)) {
                            return std::unexpected(
                                rei::error{core::make_error_code(core::error_code::invalid_argument),
                                           "can't switch between special and non-special schemes"});
                        }
                        if ((detail::includes_credentials(url) || url.port) && buffer == "file") {
                            return std::unexpected(
                                rei::error{core::make_error_code(core::error_code::invalid_argument),
                                           "file URLs can't have credentials or a port"});
                        }
                        if (url.scheme == "file" && url.host && url.host->empty()) {
                            return std::unexpected(
                                rei::error{core::make_error_code(core::error_code::invalid_argument),
                                           "file URLs with an empty host can't change scheme"});
                        }
                    }

                    set_scheme(buffer);
                    if (state_override) {
                        if (url.port == detail::default_port(url.scheme)) {
                            url.port.reset();
                        }

                        return {};
                    }

                    buffer.clear();
                    if (url.scheme == "file") {
                        state = parse_state::file;
                    } else if (special && base != nullptr && base->scheme == url.scheme) {
                        state = parse_state::special_relative_or_authority;
                    } else if (special) {
                        state = parse_state::special_authority_slashes;
                    } else if (remaining().starts_with('/')) {
                        state = parse_state::path_or_authority;
                        pointer++;
                    } else {
                        url.path.clear();
                        state = parse_state::opaque_path;
                    }
                } else if (!state_override) {
                    buffer.clear();
                    state = parse_state::no_scheme;
                    pointer = -1;
                } else {
                    return std::unexpected(rei::error{
                        core::make_error_code(core::error_code::invalid_argument), "invalid scheme"});
                }
                break;

            case parse_state::no_scheme:
                if (base == nullptr || (detail::has_opaque_path(*base) && unit != '#')) {
                    return std::unexpected(
                        rei::error{core::make_error_code(core::error_code::invalid_argument),
                                   "relative URL without a usable base"});
                }

                if (detail::has_opaque_path(*base)) {
                    set_scheme(base->scheme);
                    url.path = base->path;
                    url.query = base->query;
                    url.fragment.emplace();
                    state = parse_state::fragment;
                } else {
                    state = base->scheme == "file" ? parse_state::file : parse_state::relative;
                    pointer--;
                }
                break;

            case parse_state::special_relative_or_authority:
                if (unit == '/' && remaining().starts_with('/')) {
                    state = parse_state::special_authority_ignore_slashes;
                    pointer++;
                } else {
                    state = parse_state::relative;
                    pointer--;
                }
                break;

            case parse_state::path_or_authority:
                if (unit == '/') {
                    state = parse_state::authority;
                } else {
                    state = parse_state::path;
                    pointer--;
                }
                break;

            case parse_state::relative:
                set_scheme(base->scheme);
                if (unit == '/' || (special && unit == '\\')) {
                    state = parse_state::relative_slash;
                } else {
                    copy_authority();
                    url.path = base->path;
                    url.query = base->query;

                    if (unit == '?') {
                        url.query.emplace();
                        state = parse_state::query;
                    } else if (unit == '#') {
                        url.fragment.emplace();
                        state = parse_state::fragment;
                    } else if (unit != eof) {
                        url.query.reset();
                        detail::shorten_path(url);
                        state = parse_state::path;
                        pointer--;
                    }
                }
                break;

            case parse_state::relative_slash:
                if (special && (unit == '/' || unit == '\\')) {
                    state = parse_state::special_authority_ignore_slashes;
                } else if (unit == '/') {
                    state = parse_state::authority;
                } else {
                    copy_authority();
                    state = parse_state::path;
                    pointer--;
                }
                break;

            case parse_state::special_authority_slashes:
                state = parse_state::special_authority_ignore_slashes;
                if (unit == '/' && remaining().starts_with('/')) {
                    pointer++;
                } else {
                    pointer--;
                }
                break;

            case parse_state::special_authority_ignore_slashes:
                if (unit != '/' && unit != '\\') {
                    state = parse_state::authority;
                    pointer--;
                }
                break;

            case parse_state::authority:
                if (unit == '@') {
                    if (at_sign_seen) {
                        buffer.insert(0, "%40");
                    }

                    at_sign_seen = true;
                    for (char code_point : buffer) {
                        if (code_point == ':' && !password_token_seen) {
                            password_token_seen = true;
                            continue;
                        }

                        detail::percent_encode(password_token_seen ? url.password : url.username, code_point,
                                               encode_set::userinfo);
                    }

                    buffer.clear();
                } else if (unit == eof || unit == '/' || unit == '?' || unit == '#' ||
                           (special && unit == '\\')) {
                    if (at_sign_seen && buffer.empty()) {
                        return std::unexpected(rei::error{
                            core::make_error_code(core::error_code::invalid_argument), "missing host"});
                    }

                    pointer -= std::ssize(buffer) + 1;
                    buffer.clear();
                    state = parse_state::host;
                } else {
                    buffer += static_cast<char>(unit);
                }
                break;

            case parse_state::host:
                if (state_override && url.scheme == "file") {
                    state = parse_state::file_host;
                    pointer--;
                } else if (unit == ':' && !inside_brackets) {
                    if (buffer.empty()) {
                        return std::unexpected(rei::error{
                            core::make_error_code(core::error_code::invalid_argument), "missing host"});
                    }
                    if (state_override) {
                        return std::unexpected(
                            rei::error{core::make_error_code(core::error_code::invalid_argument),
                                       "host can't include a port"});
                    }

                    auto host = detail::parse_host(buffer, !special);
                    if (!host) {
                        return std::unexpected(host.error());
                    }

                    url.host = std::move(*host);
                    buffer.clear();
                    state = parse_state::port;
                } else if (unit == eof || unit == '/' || unit == '?' || unit == '#' ||
                           (special && unit == '\\')) {
                    pointer--;
                    if (special && buffer.empty()) {
                        return std::unexpected(rei::error{
                            core::make_error_code(core::error_code::invalid_argument), "missing host"});
                    }
                    if (state_override && buffer.empty() && (detail::includes_credentials(url) || url.port)) {
                        return std::unexpected(
                            rei::error{core::make_error_code(core::error_code::invalid_argument),
                                       "URLs with credentials or a port need a host"});
                    }

                    auto host = detail::parse_host(buffer, !special);
                    if (!host) {
                        return std::unexpected(host.error());
                    }

                    url.host = std::move(*host);
                    buffer.clear();
                    state = parse_state::path_start;
                    if (state_override) {
                        return {};
                    }
                } else {
                    if (unit == '[') {
                        inside_brackets = true;
                    }
                    if (unit == ']') {
                        inside_brackets = false;
                    }

                    buffer += static_cast<char>(unit);
                }
                break;

            case parse_state::port:
                if (is_ascii_digit(unit)) {
                    buffer += static_cast<char>(unit);
                } else if (unit == eof || unit == '/' || unit == '?' || unit == '#' ||
                           (special && unit == '\\')) {
                    if (!buffer.empty()) {
                        std::uint16_t port{};
                        if (std::from_chars(buffer.data(), buffer.data() + buffer.size(), port).ec !=
                            std::errc{}) {
                            return std::unexpected(
                                rei::error{core::make_error_code(core::error_code::invalid_argument),
                                           "port is out of range"});
                        }

                        url.port = port;
                        if (url.port == detail::default_port(url.scheme)) {
                            url.port.reset();
                        }

                        buffer.clear();
                    }

                    state = parse_state::path_start;
                    pointer--;
                } else {
                    return std::unexpected(rei::error{
                        core::make_error_code(core::error_code::invalid_argument), "port is not a number"});
                }
                break;

            case parse_state::file:
                set_scheme("file");
                url.host.emplace();
                if (unit == '/' || unit == '\\') {
                    state = parse_state::file_slash;
                } else if (base != nullptr && base->scheme == "file") {
                    url.host = base->host;
                    url.path = base->path;
                    url.query = base->query;

                    if (unit == '?') {
                        url.query.emplace();
                        state = parse_state::query;
                    } else if (unit == '#') {
                        url.fragment.emplace();
                        state = parse_state::fragment;
                    } else if (unit != eof) {
                        url.query.reset();
                        if (!detail::starts_with_windows_drive_letter(
                                std::string_view{input}.substr(pointer))) {
                            detail::shorten_path(url);
                        } else {
                            url.path.clear();
                        }

                        state = parse_state::path;
                        pointer--;
                    }
                } else {
                    state = parse_state::path;
                    pointer--;
                }
                break;

            case parse_state::file_slash:
                if (unit == '/' || unit == '\\') {
                    state = parse_state::file_host;
                } else {
                    if (base != nullptr && base->scheme == "file") {
                        url.host = base->host;
                        if (!detail::starts_with_windows_drive_letter(
                                std::string_view{input}.substr(pointer)) &&
                            detail::first_segment_is_drive_letter(base->path)) {
                            url.path += base->path.substr(0, 3);
                        }
                    }

                    state = parse_state::path;
                    pointer--;
                }
                break;

            case parse_state::file_host:
                if (unit == eof || unit == '/' || unit == '\\' || unit == '?' || unit == '#') {
                    pointer--;
                    if (!state_override && detail::is_windows_drive_letter(buffer)) {
                        state = parse_state::path;
                    } else if (buffer.empty()) {
                        url.host.emplace();
                        if (state_override) {
                            return {};
                        }

                        state = parse_state::path_start;
                    } else {
                        auto host = detail::parse_host(buffer, !special);
                        if (!host) {
                            return std::unexpected(host.error());
                        }

                        url.host = *host == "localhost" ? std::string{} : std::move(*host);
                        if (state_override) {
                            return {};
                        }

                        buffer.clear();
                        state = parse_state::path_start;
                    }
                } else {
                    buffer += static_cast<char>(unit);
                }
                break;

            case parse_state::path_start:
                if (special) {
                    state = parse_state::path;
                    if (unit != '/' && unit != '\\') {
                        pointer--;
                    }
                } else if (!state_override && unit == '?') {
                    url.query.emplace();
                    state = parse_state::query;
                } else if (!state_override && unit == '#') {
                    url.fragment.emplace();
                    state = parse_state::fragment;
                } else if (unit != eof) {
                    state = parse_state::path;
                    if (unit != '/') {
                        pointer--;
                    }
                } else if (state_override && !url.host) {
                    url.path += '/';
                }
                break;

            case parse_state::path:
                if (unit == eof || unit == '/' || (special && unit == '\\') ||
                    (!state_override && (unit == '?' || unit == '#'))) {
                    const bool slash = unit == '/' || (special && unit == '\\');
                    if (detail::is_double_dot_segment(buffer)) {
                        detail::shorten_path(url);
                        if (!slash) {
                            url.path += '/';
                        }
                    } else if (detail::is_single_dot_segment(buffer)) {
                        if (!slash) {
                            url.path += '/';
                        }
                    } else {
                        if (url.scheme == "file" && url.path.empty() &&
                            detail::is_windows_drive_letter(buffer)) {
                            buffer[1] = ':';
                        }

                        url.path += '/';
                        url.path += buffer;
                    }

                    buffer.clear();
                    if (unit == '?') {
                        url.query.emplace();
                        state = parse_state::query;
                    }
                    if (unit == '#') {
                        url.fragment.emplace();
                        state = parse_state::fragment;
                    }
                } else {
                    detail::percent_encode(buffer, static_cast<char>(unit), encode_set::path);
                }
                break;

            case parse_state::opaque_path:
                if (unit == '?') {
                    url.query.emplace();
                    state = parse_state::query;
                } else if (unit == '#') {
                    url.fragment.emplace();
                    state = parse_state::fragment;
                } else if (unit == ' ') {
                    url.path += remaining().starts_with('?') || remaining().starts_with('#') ? "%20" : " ";
                } else if (unit != eof) {
                    detail::percent_encode(url.path, static_cast<char>(unit), encode_set::c0_control);
                }
                break;

            case parse_state::query:
                if (!state_override && unit == '#') {
                    url.fragment.emplace();
                    state = parse_state::fragment;
                } else if (unit != eof) {
                    detail::percent_encode(*url.query, static_cast<char>(unit),
                                           special ? encode_set::special_query : encode_set::query);
                }
                break;

            case parse_state::fragment:
                if (unit != eof) {
                    detail::percent_encode(*url.fragment, static_cast<char>(unit), encode_set::fragment);
                }
                break;
            }

            if (pointer >= size) {
                return {};
            }
        }
    }

    auto parse_url(std::string_view input, const detail::url_record_view* base)
        -> std::expected<detail::url_record, rei::error> {
        detail::url_record url;
        return detail::basic_parse(input, base, url).transform([&] -> detail::url_record {
            return std::move(url);
        });
    }
} // namespace rei::core::detail
