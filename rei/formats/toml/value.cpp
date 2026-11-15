#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "rei/formats/toml.hpp"

namespace rei::formats::toml {
    namespace {
        template <typename Stored, typename Result = Stored>
        auto read(auto& data) noexcept -> std::optional<Result> {
            if (!std::holds_alternative<Stored>(data)) {
                return std::nullopt;
            }

            return Result{std::get<Stored>(data)};
        }
    } // namespace

    auto value::kind() const noexcept -> toml::kind {
        return static_cast<toml::kind>(m_data.index());
    }

    auto value::line() const noexcept -> std::optional<std::uint32_t> {
        return m_line;
    }

    auto value::line(std::optional<std::uint32_t> line) noexcept -> void {
        m_line = line;
    }

    auto value::as_string() const noexcept -> std::optional<std::string_view> {
        return read<std::string, std::string_view>(m_data);
    }

    auto value::as_integer() const noexcept -> std::optional<std::int64_t> {
        return read<std::int64_t>(m_data);
    }

    auto value::as_floating() const noexcept -> std::optional<double> {
        const auto integer = as_integer();
        if (!integer) {
            return read<double>(m_data);
        }

        const auto converted = static_cast<double>(*integer);
        const auto exact = converted < 0x1p63 && static_cast<std::int64_t>(converted) == *integer;
        return exact ? std::optional{converted} : std::nullopt;
    }

    auto value::as_boolean() const noexcept -> std::optional<bool> {
        return read<bool>(m_data);
    }

    auto value::as_date() const noexcept -> std::optional<toml::date> {
        return read<toml::date>(m_data);
    }

    auto value::as_time() const noexcept -> std::optional<toml::time> {
        return read<toml::time>(m_data);
    }

    auto value::as_date_time() const noexcept -> std::optional<toml::date_time> {
        return read<toml::date_time>(m_data);
    }

    auto value::as_array() const noexcept -> std::optional<std::span<const toml::value>> {
        return read<toml::array, std::span<const toml::value>>(m_data);
    }

    auto value::as_table() const noexcept -> std::optional<std::reference_wrapper<const toml::table>> {
        return read<toml::table, std::reference_wrapper<const toml::table>>(m_data);
    }

    auto value::edit_array() noexcept -> std::optional<std::reference_wrapper<toml::array>> {
        return read<toml::array, std::reference_wrapper<toml::array>>(m_data);
    }

    auto value::edit_table() noexcept -> std::optional<std::reference_wrapper<toml::table>> {
        return read<toml::table, std::reference_wrapper<toml::table>>(m_data);
    }

    auto value::operator==(const value& other) const -> bool {
        return m_data == other.m_data;
    }
} // namespace rei::formats::toml
