#include <charconv>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "rei/core/ascii.hpp"
#include "rei/core/error.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/detail.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"

namespace rei::net::http::h1 {
    namespace {
        // Anything two parsers could read differently is an error.
        auto declared_framing(http::version version, std::span<const http::header> headers,
                              h1::body_kind fallback) -> std::expected<h1::body_framing, rei::error> {
            std::optional<std::string_view> content_length;
            std::optional<std::string_view> transfer_encoding;
            auto repeated_transfer_encoding = false;

            for (const auto& header : headers) {
                if (core::ascii_iequals(header.name, "content-length")) {
                    if (content_length) {
                        return detail::failure(http::error_code::bad_framing, "repeated Content-Length");
                    }

                    content_length = header.value;
                } else if (core::ascii_iequals(header.name, "transfer-encoding")) {
                    repeated_transfer_encoding = transfer_encoding.has_value();
                    transfer_encoding = header.value;
                }
            }

            if (transfer_encoding) {
                if (content_length) {
                    return detail::failure(http::error_code::bad_framing,
                                           "Content-Length together with Transfer-Encoding");
                }
                if (version == http::version::http_1_0) {
                    return detail::failure(http::error_code::bad_framing, "Transfer-Encoding in HTTP/1.0");
                }

                // RFC 9112 §6.3: without chunked as the final coding, the length can't be determined.
                const auto last_comma = transfer_encoding->rfind(',');
                const auto final_start = last_comma == std::string_view::npos ? 0uz : last_comma + 1;
                if (!core::ascii_iequals(detail::trim(transfer_encoding->substr(final_start)), "chunked")) {
                    return detail::failure(http::error_code::bad_framing, "chunked is not the final coding");
                }
                if (repeated_transfer_encoding || last_comma != std::string_view::npos) {
                    return detail::failure(http::error_code::unsupported_transfer_coding,
                                           "only chunked is supported");
                }

                return h1::body_framing{.kind = h1::body_kind::chunked};
            }

            if (content_length) {
                std::uint64_t size{};
                const auto* const end = content_length->data() + content_length->size();
                const auto [stop, error] = std::from_chars(content_length->data(), end, size);
                if (error != std::errc{} || stop != end) {
                    return detail::failure(http::error_code::bad_framing, "invalid Content-Length");
                }

                return h1::body_framing{.kind = h1::body_kind::sized, .size = size};
            }

            return h1::body_framing{.kind = fallback};
        }
    } // namespace

    auto request_framing(const http::request_head& head) -> std::expected<h1::body_framing, rei::error> {
        return declared_framing(head.version, head.headers, h1::body_kind::none);
    }

    auto response_framing(const http::response_head& head, std::string_view request_method)
        -> std::expected<h1::body_framing, rei::error> {
        const auto status_class = std::to_underlying(head.status) / 100;
        const auto never_has_body = request_method == http::to_string(http::method::head) ||
                                    status_class == 1 || head.status == http::status::no_content ||
                                    head.status == http::status::not_modified;
        const auto opens_tunnel =
            request_method == http::to_string(http::method::connect) && status_class == 2;
        if (never_has_body || opens_tunnel) {
            return h1::body_framing{.kind = h1::body_kind::none};
        }

        return declared_framing(head.version, head.headers, h1::body_kind::until_close);
    }

    auto keeps_alive(http::version version, std::span<const http::header> headers) noexcept -> bool {
        auto has_close = false;
        auto has_keep_alive = false;

        for (const auto& header : headers) {
            if (!core::ascii_iequals(header.name, "connection")) {
                continue;
            }

            for (const auto element : std::views::split(header.value, ',')) {
                const auto option = detail::trim(std::string_view{element});

                has_close = has_close || core::ascii_iequals(option, "close");
                has_keep_alive = has_keep_alive || core::ascii_iequals(option, "keep-alive");
            }
        }

        if (has_close) {
            return false;
        }

        return version == http::version::http_1_1 || has_keep_alive;
    }
} // namespace rei::net::http::h1
