#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::net {
    enum class error_code {
        connection_closed = 1,
        timed_out,
        protocol_error,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.net"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<net::error_code>(code)) {
                case error_code::connection_closed:
                    return "connection closed";
                case error_code::timed_out:
                    return "connection timed out";
                case error_code::protocol_error:
                    return "protocol error";
                }

                return "unknown network error";
            }

            [[nodiscard]] auto default_error_condition(int code) const noexcept
                -> std::error_condition override {
                switch (static_cast<net::error_code>(code)) {
                case error_code::connection_closed:
                    return std::make_error_condition(std::errc::not_connected);
                case error_code::timed_out:
                    return std::make_error_condition(std::errc::timed_out);
                case error_code::protocol_error:
                    return std::make_error_condition(std::errc::protocol_error);
                default:
                    return {code, *this};
                }
            }
        };
    } // namespace detail

    inline auto make_error_code(net::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::net

namespace std {
    template <> class is_error_code_enum<rei::net::error_code> : public true_type {};
} // namespace std
