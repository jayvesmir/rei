#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/formats/dictionary.hpp"

namespace rei::formats::toml {
    enum class kind : std::uint8_t {
        string,
        integer,
        floating,
        boolean,
        date,
        time,
        date_time,
        array,
        table,
    };

    using date = std::chrono::year_month_day;

    struct time {
        std::uint8_t hour{};
        std::uint8_t minute{};
        std::uint8_t second{};
        std::uint32_t nanosecond{};

        auto operator==(const time&) const -> bool = default;
    };

    struct date_time {
        toml::date date{};
        toml::time time{};
        std::optional<std::chrono::minutes> offset{};

        auto operator==(const date_time&) const -> bool = default;
    };

    class value;
    using array = std::vector<toml::value>;
    using table = formats::dictionary<toml::value>;

    namespace detail {
        template <typename T>
        concept text = std::convertible_to<const T&, std::string_view>;

        template <typename T, typename... Others>
        concept one_of = (std::same_as<T, Others> || ...);

        template <typename T>
        concept integer =
            std::integral<T> && !detail::one_of<T, bool, char, wchar_t, char8_t, char16_t, char32_t> &&
            std::in_range<std::int64_t>(std::numeric_limits<T>::max());
    } // namespace detail

    class value {
      private:
        using storage = std::variant<std::string, std::int64_t, double, bool, toml::date, toml::time,
                                     toml::date_time, toml::array, toml::table>;

        storage m_data;
        std::optional<std::uint32_t> m_line;

      public:
        value(std::string text) noexcept : m_data(std::move(text)) {}

        template <detail::text Text>
        value(const Text& text) : m_data(std::in_place_type<std::string>, std::string_view{text}) {}

        template <detail::integer Integer>
        value(Integer number) noexcept : m_data(static_cast<std::int64_t>(number)) {}

        template <std::floating_point Float>
        value(Float number) noexcept : m_data(static_cast<double>(number)) {}

        template <std::same_as<bool> Boolean> value(Boolean flag) noexcept : m_data(flag) {}

        value(toml::date day) noexcept : m_data(day) {}
        value(toml::time clock) noexcept : m_data(clock) {}
        value(toml::date_time moment) noexcept : m_data(moment) {}
        value(toml::array items) noexcept : m_data(std::move(items)) {}
        value(toml::table entries) noexcept : m_data(std::move(entries)) {}

        [[nodiscard]] auto kind() const noexcept -> toml::kind;
        [[nodiscard]] auto line() const noexcept -> std::optional<std::uint32_t>;
        auto line(std::optional<std::uint32_t> line) noexcept -> void;

        [[nodiscard]] auto as_string() const noexcept -> std::optional<std::string_view>;
        [[nodiscard]] auto as_integer() const noexcept -> std::optional<std::int64_t>;
        [[nodiscard]] auto as_floating() const noexcept -> std::optional<double>;
        [[nodiscard]] auto as_boolean() const noexcept -> std::optional<bool>;
        [[nodiscard]] auto as_date() const noexcept -> std::optional<toml::date>;
        [[nodiscard]] auto as_time() const noexcept -> std::optional<toml::time>;
        [[nodiscard]] auto as_date_time() const noexcept -> std::optional<toml::date_time>;

        [[nodiscard]] auto as_array() const noexcept -> std::optional<std::span<const toml::value>>;
        [[nodiscard]] auto as_table() const noexcept
            -> std::optional<std::reference_wrapper<const toml::table>>;

        [[nodiscard]] auto edit_array() noexcept -> std::optional<std::reference_wrapper<toml::array>>;
        [[nodiscard]] auto edit_table() noexcept -> std::optional<std::reference_wrapper<toml::table>>;

        template <std::convertible_to<std::string_view>... Keys>
        [[nodiscard]] auto find(std::string_view key, const Keys&... rest) const noexcept
            -> std::optional<std::reference_wrapper<const toml::value>> {
            return as_table().and_then(
                [&](const toml::table& entries) -> std::optional<std::reference_wrapper<const toml::value>> {
                    return entries.find(key, rest...);
                });
        }

        auto operator==(const value& other) const -> bool;
    };

    auto parse(std::string_view text) -> std::expected<toml::table, rei::error>;

    auto serialize(const toml::table& document) -> std::expected<std::string, rei::error>;
    auto serialize(const toml::value& value) -> std::expected<std::string, rei::error>;
} // namespace rei::formats::toml
