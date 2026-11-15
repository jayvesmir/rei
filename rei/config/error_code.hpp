#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::config {
    enum class error_code {
        invalid_schema = 1,
        unknown_option,
        unknown_key,
        missing_value,
        invalid_value,
        missing_required,
        unexpected_argument,
        help_requested,
        version_requested,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.config"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<config::error_code>(code)) {
                case error_code::invalid_schema:
                    return "invalid option schema";
                case error_code::unknown_option:
                    return "unknown option";
                case error_code::unknown_key:
                    return "unknown key";
                case error_code::missing_value:
                    return "option requires a value";
                case error_code::invalid_value:
                    return "invalid value";
                case error_code::missing_required:
                    return "missing required option";
                case error_code::unexpected_argument:
                    return "unexpected argument";
                case error_code::help_requested:
                    return "help requested";
                case error_code::version_requested:
                    return "version requested";
                }

                return "unknown configuration error";
            }

            [[nodiscard]] auto default_error_condition(int code) const noexcept
                -> std::error_condition override {
                switch (static_cast<config::error_code>(code)) {
                case error_code::help_requested:
                case error_code::version_requested:
                    return {code, *this};
                default:
                    return std::make_error_condition(std::errc::invalid_argument);
                }
            }
        };
    } // namespace detail

    inline auto make_error_code(config::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::config

namespace std {
    template <> class is_error_code_enum<rei::config::error_code> : public true_type {};
} // namespace std
