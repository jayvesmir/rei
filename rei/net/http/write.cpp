#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <limits>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/detail.hpp"
#include "rei/net/http/h1.hpp"

namespace rei::net::http::h1 {
    namespace {
        using detail::char_class;

        constexpr std::string_view line_break = "\r\n";

        auto invalid(std::string detail, std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              std::move(detail), location});
        }

        // Grows `out` once for all the parts, which costs less than appending them one by one.
        auto append(std::vector<std::byte>& out, std::initializer_list<std::string_view> parts) -> void {
            auto size = 0uz;
            for (const auto part : parts) {
                size += part.size();
            }

            auto position = out.size();
            out.resize(position + size);

            for (const auto part : parts) {
                std::ranges::copy(std::as_bytes(std::span{part}), std::span{out}.subspan(position).begin());
                position += part.size();
            }
        }

        auto append_number(std::vector<std::byte>& out, std::size_t number, int base) -> void {
            std::array<char, std::numeric_limits<std::size_t>::digits10 + 1> digits{};
            const auto* const end =
                std::to_chars(digits.data(), digits.data() + digits.size(), number, base).ptr;

            append(out, {std::string_view{digits.data(), end}});
        }

        auto validate_headers(std::span<const http::header> headers) -> std::expected<void, rei::error> {
            for (const auto& header : headers) {
                if (header.name.empty() || !detail::consists_of(header.name, char_class::token)) {
                    return invalid("invalid header name");
                }
                if (!detail::consists_of(header.value, char_class::value)) {
                    return invalid("invalid header value");
                }
            }

            return {};
        }

        auto append_headers(std::vector<std::byte>& out, std::span<const http::header> headers) -> void {
            for (const auto& header : headers) {
                append(out, {header.name, ": ", header.value, line_break});
            }
        }
    } // namespace

    auto serialize_head(const http::request_head& head, std::vector<std::byte>& out)
        -> std::expected<void, rei::error> {
        if (head.method.empty() || !detail::consists_of(head.method, char_class::token)) {
            return invalid("invalid method");
        }
        if (head.target.empty() || !detail::consists_of(head.target, char_class::target)) {
            return invalid("invalid request target");
        }
        const auto version = detail::version_text(head.version);
        if (version.empty()) {
            return invalid("invalid version");
        }
        if (auto valid = validate_headers(head.headers); !valid) {
            return valid;
        }

        append(out, {head.method, " ", head.target, " ", version, line_break});
        append_headers(out, head.headers);
        append(out, {line_break});

        return {};
    }

    auto serialize_head(const http::response_head& head, std::vector<std::byte>& out)
        -> std::expected<void, rei::error> {
        const auto version = detail::version_text(head.version);
        if (version.empty()) {
            return invalid("invalid version");
        }
        const auto code = std::to_underlying(head.status);
        if (code < 100 || code > 999) {
            return invalid("status code is not three digits");
        }
        if (!detail::consists_of(head.reason, char_class::value)) {
            return invalid("invalid reason phrase");
        }
        if (auto valid = validate_headers(head.headers); !valid) {
            return valid;
        }

        append(out, {version, " "});
        append_number(out, code, 10);
        append(out, {" ", head.reason.empty() ? http::reason_phrase(head.status) : head.reason, line_break});
        append_headers(out, head.headers);
        append(out, {line_break});

        return {};
    }

    auto write_chunk(std::span<const std::byte> data, std::vector<std::byte>& out) -> void {
        if (data.empty()) {
            return;
        }

        append_number(out, data.size(), 16);
        append(out, {line_break});
        out.insert(out.end(), data.begin(), data.end());
        append(out, {line_break});
    }

    auto write_last_chunk(std::vector<std::byte>& out) -> void {
        append(out, {"0\r\n\r\n"});
    }
} // namespace rei::net::http::h1
