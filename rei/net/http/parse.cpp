#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "rei/core/error.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/detail.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"

namespace rei::net::http::h1 {
    namespace {
        using detail::char_class;

        constexpr auto version_size = detail::version_text(http::version::http_1_1).size();

        constexpr auto is_digit(char unit) -> bool {
            return unit >= '0' && unit <= '9';
        }

        auto parse_version(std::string_view text) -> std::expected<http::version, rei::error> {
            for (const auto version : {http::version::http_1_1, http::version::http_1_0}) {
                if (text == detail::version_text(version)) {
                    return version;
                }
            }

            const auto is_other_version = text.size() == version_size && text.starts_with("HTTP/") &&
                                          is_digit(text[5]) && text[6] == '.' && is_digit(text[7]);
            if (is_other_version) {
                return detail::failure(http::error_code::unsupported_version, "only HTTP/1.0 and HTTP/1.1");
            }

            return detail::failure(http::error_code::bad_start_line, "invalid HTTP version");
        }

        // method SP request-target SP HTTP-version
        auto parse_start_line(std::string_view line, http::request_head& head)
            -> std::expected<void, rei::error> {
            const auto method_end = detail::skip(line, 0, char_class::token);
            if (method_end == 0 || method_end == line.size() || line[method_end] != ' ') {
                return detail::failure(http::error_code::bad_start_line, "invalid method");
            }

            const auto target_start = method_end + 1;
            const auto target_end = detail::skip(line, target_start, char_class::target);
            if (target_end == target_start || target_end == line.size() || line[target_end] != ' ') {
                return detail::failure(http::error_code::bad_start_line, "invalid request target");
            }

            const auto version = parse_version(line.substr(target_end + 1));
            if (!version) {
                return std::unexpected(version.error());
            }

            head.method = line.substr(0, method_end);
            head.target = line.substr(target_start, target_end - target_start);
            head.version = *version;

            return {};
        }

        // HTTP-version SP status-code [SP [reason-phrase]]
        auto parse_start_line(std::string_view line, http::response_head& head)
            -> std::expected<void, rei::error> {
            const auto version = parse_version(line.substr(0, version_size));
            if (!version) {
                return std::unexpected(version.error());
            }

            const auto code_start = version_size + 1;
            const auto code_end = code_start + 3;
            const auto has_code = line.size() >= code_end && line[version_size] == ' ' &&
                                  line[code_start] >= '1' && line[code_start] <= '9' &&
                                  is_digit(line[code_start + 1]) && is_digit(line[code_start + 2]);
            if (!has_code) {
                return detail::failure(http::error_code::bad_start_line, "invalid status code");
            }

            // Servers do send "HTTP/1.1 200" without the space before an empty reason.
            auto reason = line.substr(code_end);
            if (!reason.empty()) {
                if (reason.front() != ' ') {
                    return detail::failure(http::error_code::bad_start_line, "invalid status code");
                }

                reason.remove_prefix(1);
            }
            if (!detail::consists_of(reason, char_class::value)) {
                return detail::failure(http::error_code::bad_start_line, "invalid reason phrase");
            }

            const auto code = ((line[code_start] - '0') * 100) + ((line[code_start + 1] - '0') * 10) +
                              (line[code_start + 2] - '0');

            head.version = *version;
            head.status = static_cast<http::status>(code);
            head.reason = reason;

            return {};
        }

        // field-name ":" OWS field-value OWS
        auto parse_header(std::string_view line) -> std::expected<http::header, rei::error> {
            const auto name_end = detail::skip(line, 0, char_class::token);
            if (name_end == 0 || name_end == line.size() || line[name_end] != ':') {
                return detail::failure(http::error_code::bad_header, "invalid header name");
            }

            const auto value = detail::trim(line.substr(name_end + 1));
            if (!detail::consists_of(value, char_class::value)) {
                return detail::failure(http::error_code::bad_header, "invalid header value");
            }

            return http::header{.name = line.substr(0, name_end), .value = value};
        }

        constexpr auto remove_cr(std::string_view& line) -> bool {
            if (!line.ends_with('\r')) {
                return false;
            }

            line.remove_suffix(1);
            return true;
        }

        template <typename Head>
        auto parse(std::span<const std::byte> input, Head& head, const h1::parse_limits& limits)
            -> std::expected<std::optional<std::size_t>, rei::error> {
            const std::string_view text{reinterpret_cast<const char*>(input.data()),
                                        std::min(input.size(), limits.max_head_size.value_or(input.size()))};

            const auto incomplete = [&] -> std::expected<std::optional<std::size_t>, rei::error> {
                if (limits.max_head_size && input.size() >= *limits.max_head_size) {
                    return detail::failure(http::error_code::head_too_large, "head exceeds the size limit");
                }

                return std::nullopt;
            };

            auto line_end = text.find('\n');
            if (line_end == std::string_view::npos) {
                return incomplete();
            }

            auto line = text.substr(0, line_end);
            if (!remove_cr(line)) {
                return detail::failure(http::error_code::bad_start_line, "line ends without CR");
            }
            if (auto parsed = parse_start_line(line, head); !parsed) {
                return std::unexpected(parsed.error());
            }

            head.headers.clear();

            while (true) {
                const auto line_start = line_end + 1;
                line_end = text.find('\n', line_start);
                if (line_end == std::string_view::npos) {
                    return incomplete();
                }

                line = text.substr(line_start, line_end - line_start);
                if (!remove_cr(line)) {
                    return detail::failure(http::error_code::bad_header, "line ends without CR");
                }
                if (line.empty()) {
                    return line_end + 1;
                }

                if (limits.max_headers && head.headers.size() == *limits.max_headers) {
                    return detail::failure(http::error_code::too_many_headers, "too many headers");
                }

                const auto header = parse_header(line);
                if (!header) {
                    return std::unexpected(header.error());
                }

                head.headers.push_back(*header);
            }
        }
    } // namespace

    auto parse_head(std::span<const std::byte> input, http::request_head& head,
                    const h1::parse_limits& limits) -> std::expected<std::optional<std::size_t>, rei::error> {
        return parse(input, head, limits);
    }

    auto parse_head(std::span<const std::byte> input, http::response_head& head,
                    const h1::parse_limits& limits) -> std::expected<std::optional<std::size_t>, rei::error> {
        return parse(input, head, limits);
    }
} // namespace rei::net::http::h1
