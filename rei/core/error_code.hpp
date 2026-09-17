#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::core {
    enum class error_code {
        unknown = 1,
        invalid_argument,
        not_supported,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.core"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<core::error_code>(code)) {
                case error_code::unknown:
                    return "unknown";
                case error_code::invalid_argument:
                    return "invalid argument";
                case error_code::not_supported:
                    return "operation not supported";
                }

                return "unknown core error";
            }

            [[nodiscard]] auto default_error_condition(int code) const noexcept
                -> std::error_condition override {
                switch (static_cast<core::error_code>(code)) {
                case error_code::invalid_argument:
                    return std::make_error_condition(std::errc::invalid_argument);
                case error_code::not_supported:
                    return std::make_error_condition(std::errc::not_supported);
                default:
                    return {code, *this};
                }
            }
        };
    } // namespace detail

    inline auto make_error_code(core::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::core

namespace std {
    // std::error_code compatibility
    template <> class is_error_code_enum<rei::core::error_code> : public true_type {
      private:
      public:
    };
} // namespace std
