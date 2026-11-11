#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::net::tls {
    enum class error_code {
        truncated = 1,
        certificate_rejected,
        handshake_failed,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.net.tls"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<tls::error_code>(code)) {
                case tls::error_code::truncated:
                    return "connection closed without close_notify";
                case tls::error_code::certificate_rejected:
                    return "certificate rejected";
                case tls::error_code::handshake_failed:
                    return "handshake failed";
                }

                return "unknown TLS error";
            }

            [[nodiscard]] auto default_error_condition(int /*code*/) const noexcept
                -> std::error_condition override {
                return std::make_error_condition(std::errc::protocol_error);
            }
        };
    } // namespace detail

    inline auto make_error_code(tls::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::net::tls

namespace std {
    template <> class is_error_code_enum<rei::net::tls::error_code> : public true_type {};
} // namespace std
