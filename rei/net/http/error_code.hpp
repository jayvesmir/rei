#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::net::http {
    enum class error_code {
        bad_start_line = 1,
        bad_header,
        bad_framing,
        bad_chunk,
        head_too_large,
        too_many_headers,
        unsupported_transfer_coding,
        unsupported_version,
        body_too_large,
        too_many_redirects,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.net.http"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<http::error_code>(code)) {
                case error_code::bad_start_line:
                    return "malformed request or status line";
                case error_code::bad_header:
                    return "malformed header";
                case error_code::bad_framing:
                    return "ambiguous or invalid message body length";
                case error_code::bad_chunk:
                    return "malformed chunked body";
                case error_code::head_too_large:
                    return "message head too large";
                case error_code::too_many_headers:
                    return "too many headers";
                case error_code::unsupported_transfer_coding:
                    return "unsupported transfer coding";
                case error_code::unsupported_version:
                    return "unsupported HTTP version";
                case error_code::body_too_large:
                    return "message body too large";
                case error_code::too_many_redirects:
                    return "too many redirects";
                }

                return "unknown HTTP error";
            }

            [[nodiscard]] auto default_error_condition(int /*code*/) const noexcept
                -> std::error_condition override {
                return std::make_error_condition(std::errc::protocol_error);
            }
        };
    } // namespace detail

    inline auto make_error_code(http::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::net::http

namespace std {
    template <> class is_error_code_enum<rei::net::http::error_code> : public true_type {};
} // namespace std
