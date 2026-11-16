#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::formats {
    enum class error_code {
        invalid_syntax = 1,
        depth_exceeded,
        invalid_value,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.formats"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<formats::error_code>(code)) {
                case error_code::invalid_syntax:
                    return "invalid syntax";
                case error_code::depth_exceeded:
                    return "document nests too deeply";
                case error_code::invalid_value:
                    return "value can't be written in this format";
                }

                return "unknown formats error";
            }

            [[nodiscard]] auto default_error_condition(int code) const noexcept
                -> std::error_condition override {
                switch (static_cast<formats::error_code>(code)) {
                case error_code::invalid_syntax:
                case error_code::invalid_value:
                    return std::make_error_condition(std::errc::invalid_argument);
                default:
                    return {code, *this};
                }
            }
        };
    } // namespace detail

    inline auto make_error_code(formats::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::formats

namespace std {
    template <> class is_error_code_enum<rei::formats::error_code> : public true_type {};
} // namespace std
