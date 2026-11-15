#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "rei/config/config.hpp"
#include "rei/config/detail.hpp"
#include "rei/core/ascii.hpp"

namespace rei::config {
    namespace {
        constexpr std::string_view digits = "0123456789";

        auto is_integer(std::string_view text) -> bool {
            if (text.starts_with('-')) {
                text.remove_prefix(1);
            }

            return !text.empty() && text.find_first_not_of(digits) == std::string_view::npos;
        }

        auto not_a_number(std::string_view text) -> std::unexpected<std::string> {
            return std::unexpected(std::format("'{}' is not a number", text));
        }

        template <std::integral T>
        auto out_of_range(std::string_view text, T lowest, T highest) -> std::unexpected<std::string> {
            return std::unexpected(std::format("{} is out of range ({} to {})", text, lowest, highest));
        }

        auto unit_named(std::string_view suffix) -> std::optional<detail::duration_unit> {
            for (const auto& unit : detail::duration_units) {
                if (unit.suffix == suffix) {
                    return unit;
                }
            }

            return std::nullopt;
        }

        template <std::floating_point T>
        auto parse_floating(std::string_view text) -> std::expected<T, std::string> {
            T value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

            if (error == std::errc::result_out_of_range) {
                return std::unexpected(std::format("{} is out of range", text));
            }
            if (error != std::errc{} || end != text.data() + text.size()) {
                return not_a_number(text);
            }

            return value;
        }
    } // namespace

    auto detail::parse_signed(std::string_view text, std::intmax_t lowest, std::intmax_t highest)
        -> std::expected<std::intmax_t, std::string> {
        if (!is_integer(text)) {
            return not_a_number(text);
        }

        std::intmax_t value{};
        const auto error = std::from_chars(text.data(), text.data() + text.size(), value).ec;

        if (error != std::errc{} || value < lowest || value > highest) {
            return out_of_range(text, lowest, highest);
        }

        return value;
    }

    auto detail::parse_unsigned(std::string_view text, std::uintmax_t highest)
        -> std::expected<std::uintmax_t, std::string> {
        if (!is_integer(text)) {
            return not_a_number(text);
        }

        std::uintmax_t value{};
        const auto error = std::from_chars(text.data(), text.data() + text.size(), value).ec;

        if (error != std::errc{} || value > highest) {
            return out_of_range(text, std::uintmax_t{}, highest);
        }

        return value;
    }

    auto detail::parse_duration(std::string_view text, const detail::duration_unit& target,
                                std::uintmax_t highest) -> std::expected<std::uintmax_t, std::string> {
        const auto malformed = [&] -> std::unexpected<std::string> {
            return std::unexpected(std::format("'{}' is not a duration such as 1m30s", text));
        };
        const auto too_long = [&] -> std::unexpected<std::string> {
            return std::unexpected(
                std::format("{} is out of range (up to {}{})", text, highest, target.suffix));
        };

        auto rest = text;
        std::uintmax_t total{};

        do {
            const auto number = rest.substr(0, rest.find_first_not_of(digits));
            rest.remove_prefix(number.size());
            const auto suffix = rest.substr(0, rest.find_first_of(digits));
            rest.remove_prefix(suffix.size());

            std::uintmax_t count{};
            const auto error = std::from_chars(number.data(), number.data() + number.size(), count).ec;
            const auto unit = unit_named(suffix);

            if (error == std::errc::result_out_of_range) {
                return too_long();
            }
            if (error != std::errc{} || !unit) {
                return malformed();
            }

            const auto size = static_cast<std::uintmax_t>(unit->nanoseconds);
            const auto period = static_cast<std::uintmax_t>(target.nanoseconds);

            if (size < period) {
                if (count % (period / size) != 0) {
                    return std::unexpected(std::format("{} is not a multiple of 1{}", text, target.suffix));
                }

                count /= period / size;
            } else {
                if (count > highest / (size / period)) {
                    return too_long();
                }

                count *= size / period;
            }

            if (count > highest - total) {
                return too_long();
            }

            total += count;
        } while (!rest.empty());

        return total;
    }

    auto parser<bool>::parse(std::string_view text) -> std::expected<bool, std::string> {
        constexpr auto truthy = std::array<std::string_view, 4>{"true", "1", "yes", "on"};
        constexpr auto falsy = std::array<std::string_view, 4>{"false", "0", "no", "off"};
        const auto spelled = [&](std::string_view word) -> bool { return core::ascii_iequals(text, word); };

        if (std::ranges::any_of(truthy, spelled)) {
            return true;
        }
        if (std::ranges::any_of(falsy, spelled)) {
            return false;
        }

        return std::unexpected(std::format("'{}' is not a boolean", text));
    }

    auto parser<float>::parse(std::string_view text) -> std::expected<float, std::string> {
        return parse_floating<float>(text);
    }

    auto parser<double>::parse(std::string_view text) -> std::expected<double, std::string> {
        return parse_floating<double>(text);
    }
} // namespace rei::config
