#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "magic_enum/magic_enum.hpp"

#include "rei/config/detail.hpp"
#include "rei/core/error.hpp"

namespace rei::config {
    struct origin {
        std::string source;
        std::uint32_t line{};
    };

    struct value {
        std::string key;
        std::string text;
        config::origin origin{};
    };

    using layer = std::vector<config::value>;

    template <typename T> struct parser;

    template <typename T>
    concept parsable = requires(std::string_view text) {
        { config::parser<T>::parse(text) } -> std::same_as<std::expected<T, std::string>>;
    };

    template <typename T>
    concept bindable = config::parsable<detail::element<T>>;

    struct option_info {
        char short_name{};
        std::string_view long_name{};
        std::string_view value_name{};
        std::string_view help{};
        std::optional<std::string_view> env{};
        bool required{};
        bool hidden{};
    };

    struct command_info {
        std::string_view help{};
        std::string_view version{};
        std::string_view env_prefix{};
        std::function<auto(const rei::error&)->void> on_warning{};
    };

    class command {
      private:
        std::string m_name;
        std::string m_help;
        std::string m_version;
        std::string m_env_prefix;
        std::function<auto(const rei::error&)->void> m_on_warning;
        std::vector<detail::entry> m_entries;

        auto _add(std::string_view key, std::unique_ptr<detail::binding> target,
                  const config::option_info& info, detail::entry_kind kind) -> void;
        [[nodiscard]] auto _validate() const -> std::expected<void, rei::error>;
        [[nodiscard]] auto _builtins() const -> std::vector<detail::builtin>;
        [[nodiscard]] auto _usage() const -> std::string;

      public:
        explicit command(std::string_view name, const config::command_info& info = {});

        template <config::bindable T>
        auto option(std::string_view key, T& target, const config::option_info& info = {}) -> void {
            _add(key, std::make_unique<detail::typed_binding<T>>(target), info, detail::entry_kind::option);
        }

        template <config::bindable T>
        auto argument(std::string_view key, T& target, const config::option_info& info = {}) -> void {
            _add(key, std::make_unique<detail::typed_binding<T>>(target), info, detail::entry_kind::argument);
        }

        auto load(int argc, const char* const* argv) -> std::expected<void, rei::error>;

        auto read_args(int argc, const char* const* argv) -> std::expected<config::layer, rei::error>;
        auto read_args(std::span<const std::string_view> args) -> std::expected<config::layer, rei::error>;
        [[nodiscard]] auto read_env() const -> std::expected<config::layer, rei::error>;
        [[nodiscard]] auto read_env(std::span<const std::string_view> environment) const
            -> std::expected<config::layer, rei::error>;
        auto apply(std::span<const config::layer> layers) -> std::expected<void, rei::error>;

        [[nodiscard]] auto help() const -> std::string;
        [[nodiscard]] auto report(const rei::error& error) const -> int;
    };

    template <> struct parser<bool> {
        static auto parse(std::string_view text) -> std::expected<bool, std::string>;
    };

    template <detail::integer T> struct parser<T> {
        static auto parse(std::string_view text) -> std::expected<T, std::string> {
            constexpr auto limits = std::numeric_limits<T>{};

            if constexpr (std::signed_integral<T>) {
                return detail::parse_signed(text, limits.min(), limits.max())
                    .transform([](std::intmax_t value) -> T { return static_cast<T>(value); });
            } else {
                return detail::parse_unsigned(text, limits.max()).transform([](std::uintmax_t value) -> T {
                    return static_cast<T>(value);
                });
            }
        }
    };

    template <> struct parser<float> {
        static auto parse(std::string_view text) -> std::expected<float, std::string>;
    };

    template <> struct parser<double> {
        static auto parse(std::string_view text) -> std::expected<double, std::string>;
    };

    template <> struct parser<std::string> {
        static auto parse(std::string_view text) -> std::expected<std::string, std::string> {
            return std::string{text};
        }
    };

    template <> struct parser<std::filesystem::path> {
        static auto parse(std::string_view text) -> std::expected<std::filesystem::path, std::string> {
            return std::filesystem::path{text};
        }
    };

    template <typename T>
        requires std::is_enum_v<T>
    struct parser<T> {
        static auto parse(std::string_view text) -> std::expected<T, std::string> {
            if (const auto value = magic_enum::enum_cast<T>(text)) {
                return *value;
            }

            return std::unexpected(
                std::format("'{}' is not one of {}", text, detail::join(magic_enum::enum_names<T>(), ", ")));
        }
    };

    template <detail::integer Rep, detail::unit_period Period>
    struct parser<std::chrono::duration<Rep, Period>> {
        static auto parse(std::string_view text)
            -> std::expected<std::chrono::duration<Rep, Period>, std::string> {
            const auto highest = static_cast<std::uintmax_t>(std::numeric_limits<Rep>::max());

            return detail::parse_duration(text, *detail::unit_of<Period>, highest)
                .transform([](std::uintmax_t count) -> std::chrono::duration<Rep, Period> {
                    return std::chrono::duration<Rep, Period>{static_cast<Rep>(count)};
                });
        }
    };
} // namespace rei::config
