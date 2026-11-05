#include <algorithm>
#include <expected>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "rei/core/url.hpp"
#include "rei/core/url/detail.hpp"
#include "rei/core/utf8.hpp"

namespace rei::core::detail {
    namespace {
        auto is_valid_scheme(std::string_view scheme) -> bool {
            return !scheme.empty() && is_ascii_alpha(scheme.front()) &&
                   std::ranges::all_of(scheme, [](char unit) -> bool {
                       return is_ascii_alphanumeric(unit) || unit == '+' || unit == '-' || unit == '.';
                   });
        }

        auto cannot_have_credentials_or_port(const detail::url_record& url) -> bool {
            return !url.host || url.host->empty() || url.scheme == "file";
        }

        auto encode_opaque_path(std::string_view value) -> std::expected<std::string, rei::error> {
            if (!utf8::is_valid(value)) {
                return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                                  "path is not valid UTF-8"});
            }

            std::string path;
            for (char unit : value) {
                if (unit == '?' || unit == '#') {
                    detail::append_percent_encoded(path, unit);
                } else if (unit != '\t' && unit != '\n' && unit != '\r') {
                    detail::percent_encode(path, unit, encode_set::c0_control);
                }
            }

            if (path.ends_with(' ')) {
                path.pop_back();
                path += "%20";
            }

            return path;
        }
    } // namespace

    auto write_scheme(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error> {
        if (!detail::is_valid_scheme(value)) {
            return std::unexpected(
                rei::error{core::make_error_code(core::error_code::invalid_argument), "invalid scheme"});
        }

        return detail::basic_parse(std::string{value} + ':', nullptr, url, parse_state::scheme_start);
    }

    auto write_username(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error> {
        if (detail::cannot_have_credentials_or_port(url)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "URLs without a host and file URLs can't have credentials"});
        }
        if (!utf8::is_valid(value)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "username is not valid UTF-8"});
        }

        url.username = detail::percent_encode(value, encode_set::userinfo);
        return {};
    }

    auto write_password(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error> {
        if (detail::cannot_have_credentials_or_port(url)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "URLs without a host and file URLs can't have credentials"});
        }
        if (!utf8::is_valid(value)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "password is not valid UTF-8"});
        }

        url.password = detail::percent_encode(value, encode_set::userinfo);
        return {};
    }

    auto write_host(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error> {
        // The hostname setter stops at these and would silently drop the rest of the value.
        if (value.find_first_of("/?#\\") != std::string_view::npos) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "host contains a URL delimiter"});
        }

        return detail::basic_parse(value, nullptr, url, parse_state::host);
    }

    auto write_port(detail::url_record& url, std::optional<std::uint16_t> port)
        -> std::expected<void, rei::error> {
        if (detail::cannot_have_credentials_or_port(url)) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "URLs without a host and file URLs can't have a port"});
        }

        url.port = port == detail::default_port(url.scheme) ? std::nullopt : port;
        return {};
    }

    auto write_path(detail::url_record& url, std::string_view value) -> std::expected<void, rei::error> {
        url.path.clear();
        return detail::basic_parse(value, nullptr, url, parse_state::path_start);
    }

    auto write_query(detail::url_record& url, std::optional<std::string_view> value)
        -> std::expected<void, rei::error> {
        if (!value) {
            url.query.reset();
            return {};
        }

        url.query.emplace();
        return detail::basic_parse(*value, nullptr, url, parse_state::query);
    }

    auto write_fragment(detail::url_record& url, std::optional<std::string_view> value)
        -> std::expected<void, rei::error> {
        if (!value) {
            url.fragment.reset();
            return {};
        }

        url.fragment.emplace();
        return detail::basic_parse(*value, nullptr, url, parse_state::fragment);
    }

    auto record_from_parts(const core::url_parts& parts) -> std::expected<detail::url_record, rei::error> {
        if (!detail::is_valid_scheme(parts.scheme)) {
            return std::unexpected(
                rei::error{core::make_error_code(core::error_code::invalid_argument), "invalid scheme"});
        }

        detail::url_record url;
        url.scheme = parts.scheme | std::views::transform(to_ascii_lower) | std::ranges::to<std::string>();
        if (parts.host) {
            if (auto result = detail::write_host(url, *parts.host); !result) {
                return std::unexpected(result.error());
            }
        } else if (url.scheme == "file") {
            url.host.emplace();
        } else if (detail::is_special(url.scheme)) {
            return std::unexpected(
                rei::error{core::make_error_code(core::error_code::invalid_argument), "missing host"});
        }

        if (!parts.username.empty()) {
            if (auto result = detail::write_username(url, parts.username); !result) {
                return std::unexpected(result.error());
            }
        }

        if (!parts.password.empty()) {
            if (auto result = detail::write_password(url, parts.password); !result) {
                return std::unexpected(result.error());
            }
        }
        if (parts.port) {
            if (auto result = detail::write_port(url, parts.port); !result) {
                return std::unexpected(result.error());
            }
        }

        if (!url.host && !parts.path.starts_with('/')) {
            auto path = detail::encode_opaque_path(parts.path);
            if (!path) {
                return std::unexpected(path.error());
            }

            url.path = std::move(*path);
        } else if (auto result = detail::write_path(url, parts.path); !result) {
            return std::unexpected(result.error());
        }

        if (auto result = detail::write_query(url, parts.query); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = detail::write_fragment(url, parts.fragment); !result) {
            return std::unexpected(result.error());
        }

        return url;
    }

    auto record_from_path(const std::filesystem::path& path)
        -> std::expected<detail::url_record, rei::error> {
        if (!path.is_absolute()) {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              "file URLs need an absolute path"});
        }

        auto generic = path.generic_u8string();
        std::string input = generic.starts_with(u8'/') ? "file://" : "file:///";
        for (char8_t code_unit : generic) {
            auto unit = static_cast<char>(code_unit);
            if (unit == '%' || unit == '\\') {
                detail::append_percent_encoded(input, unit);
            } else {
                detail::percent_encode(input, unit, encode_set::path);
            }
        }

        return detail::parse_url(input, nullptr);
    }
} // namespace rei::core::detail
