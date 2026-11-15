#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "magic_enum/magic_enum.hpp"

#include "rei/config/error_code.hpp"

namespace rei::config {
    template <typename T> struct parser;

    namespace detail {
        template <typename T, template <typename...> typename Template> constexpr auto specializes = false;

        template <template <typename...> typename Template, typename... Arguments>
        constexpr auto specializes<Template<Arguments...>, Template> = true;

        template <typename T>
        concept container = detail::specializes<T, std::optional> || detail::specializes<T, std::vector>;

        template <typename T> struct element_of : std::type_identity<T> {};
        template <detail::container T> struct element_of<T> : std::type_identity<typename T::value_type> {};
        template <typename T> using element = detail::element_of<T>::type;

        template <typename T, typename... Others>
        concept one_of = (std::same_as<T, Others> || ...);

        template <typename T>
        concept integer =
            std::integral<T> && !detail::one_of<T, bool, char, wchar_t, char8_t, char16_t, char32_t>;

        struct duration_unit {
            std::string_view suffix;
            std::intmax_t nanoseconds{};
        };

        inline constexpr auto duration_units = std::to_array<detail::duration_unit>({
            {.suffix = "ns", .nanoseconds = 1},
            {.suffix = "us", .nanoseconds = 1'000},
            {.suffix = "ms", .nanoseconds = 1'000'000},
            {.suffix = "s", .nanoseconds = 1'000'000'000},
            {.suffix = "m", .nanoseconds = 60'000'000'000},
            {.suffix = "h", .nanoseconds = 3'600'000'000'000},
            {.suffix = "d", .nanoseconds = 86'400'000'000'000},
        });

        constexpr auto find_unit(std::intmax_t nanoseconds) -> std::optional<detail::duration_unit> {
            for (const auto& unit : detail::duration_units) {
                if (unit.nanoseconds == nanoseconds) {
                    return unit;
                }
            }

            return std::nullopt;
        }

        template <typename Period>
        constexpr auto unit_of = std::ratio_divide<Period, std::nano>::den == 1
                                     ? detail::find_unit(std::ratio_divide<Period, std::nano>::num)
                                     : std::nullopt;

        template <typename Period>
        concept unit_period = detail::unit_of<Period>.has_value();

        auto parse_signed(std::string_view text, std::intmax_t lowest, std::intmax_t highest)
            -> std::expected<std::intmax_t, std::string>;
        auto parse_unsigned(std::string_view text, std::uintmax_t highest)
            -> std::expected<std::uintmax_t, std::string>;
        auto parse_duration(std::string_view text, const detail::duration_unit& target,
                            std::uintmax_t highest) -> std::expected<std::uintmax_t, std::string>;

        auto join(std::span<const std::string_view> items, std::string_view separator) -> std::string;
        auto did_you_mean(std::string_view name, std::span<const std::string> candidates) -> std::string;
        auto env_name(std::string_view prefix, std::string_view key) -> std::string;

        template <typename T> auto show(const T& value) -> std::string {
            if constexpr (std::is_enum_v<T>) {
                return std::string{magic_enum::enum_name(value)};
            } else if constexpr (std::same_as<T, std::filesystem::path>) {
                return value.string();
            } else if constexpr (std::formattable<T, char>) {
                return std::format("{}", value);
            } else {
                return {};
            }
        }

        template <typename Rep, detail::unit_period Period>
        auto show(const std::chrono::duration<Rep, Period>& value) -> std::string {
            return std::format("{}{}", value.count(), detail::unit_of<Period>->suffix);
        }

        enum class binding_kind : std::uint8_t {
            flag,
            value,
            list,
        };

        struct rejection {
            std::size_t index{};
            std::string reason;
        };

        class binding {
          private:
          public:
            virtual ~binding() = default;

            virtual auto assign(std::span<const std::string_view> texts)
                -> std::expected<void, detail::rejection> = 0;
            [[nodiscard]] virtual auto shown_default() const -> std::string = 0;
            [[nodiscard]] virtual auto choices() const -> std::span<const std::string_view> = 0;
            [[nodiscard]] virtual auto kind() const noexcept -> detail::binding_kind = 0;
        };

        template <typename T> class typed_binding final : public detail::binding {
          private:
            using element = detail::element<T>;

            T& m_target;

          public:
            explicit typed_binding(T& target) noexcept : m_target(target) {}

            auto assign(std::span<const std::string_view> texts)
                -> std::expected<void, detail::rejection> override {
                if constexpr (detail::specializes<T, std::vector>) {
                    T parsed;
                    parsed.reserve(texts.size());

                    for (auto i = 0uz; i < texts.size(); i++) {
                        auto item = config::parser<element>::parse(texts[i]);
                        if (!item) {
                            return std::unexpected(
                                detail::rejection{.index = i, .reason = std::move(item).error()});
                        }

                        parsed.push_back(std::move(*item));
                    }

                    m_target = std::move(parsed);
                } else {
                    const auto last = texts.size() - 1;

                    if constexpr (detail::specializes<T, std::optional>) {
                        if (texts[last] == "none") {
                            m_target.reset();
                            return {};
                        }
                    }

                    auto parsed = config::parser<element>::parse(texts[last]);
                    if (!parsed) {
                        return std::unexpected(
                            detail::rejection{.index = last, .reason = std::move(parsed).error()});
                    }

                    m_target = std::move(*parsed);
                }

                return {};
            }

            [[nodiscard]] auto shown_default() const -> std::string override {
                if constexpr (std::same_as<T, bool>) {
                    return m_target ? "true" : "";
                } else if constexpr (detail::specializes<T, std::vector>) {
                    std::string joined;
                    for (const auto& item : m_target) {
                        joined += joined.empty() ? "" : ",";
                        joined += detail::show(item);
                    }

                    return joined;
                } else if constexpr (detail::specializes<T, std::optional>) {
                    return m_target ? detail::show(*m_target) : "none";
                } else {
                    return detail::show(m_target);
                }
            }

            [[nodiscard]] auto choices() const -> std::span<const std::string_view> override {
                if constexpr (std::is_enum_v<element>) {
                    return magic_enum::enum_names<element>();
                } else {
                    return {};
                }
            }

            [[nodiscard]] auto kind() const noexcept -> detail::binding_kind override {
                if constexpr (detail::specializes<T, std::vector>) {
                    return detail::binding_kind::list;
                } else if constexpr (std::same_as<element, bool>) {
                    return detail::binding_kind::flag;
                } else {
                    return detail::binding_kind::value;
                }
            }
        };

        enum class entry_kind : std::uint8_t {
            option,
            argument,
        };

        struct entry {
            detail::entry_kind kind{};
            std::string key;
            std::optional<std::string> long_name;
            std::optional<char> short_name;
            std::string value_name;
            std::string help;
            std::optional<std::string> env_name;
            bool env_derived{};
            bool required{};
            bool hidden{};
            std::unique_ptr<detail::binding> target;
        };

        struct builtin {
            std::optional<std::string_view> long_name;
            std::optional<char> short_name;
            std::string_view help;
            config::error_code code{};
        };
    } // namespace detail
} // namespace rei::config
