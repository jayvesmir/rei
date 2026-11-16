#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/config/config.hpp"
#include "rei/config/detail.hpp"
#include "rei/config/error_code.hpp"
#include "rei/core/ascii.hpp"
#include "rei/core/error.hpp"

namespace rei::config {
    namespace {
        auto invalid_schema(std::string detail,
                            std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{error_code::invalid_schema, std::move(detail), location});
        }

        constexpr auto is_lower(char unit) -> bool {
            return unit >= 'a' && unit <= 'z';
        }

        constexpr auto is_upper(char unit) -> bool {
            return unit >= 'A' && unit <= 'Z';
        }

        constexpr auto is_digit(char unit) -> bool {
            return unit >= '0' && unit <= '9';
        }

        constexpr auto is_segment(std::string_view text) -> bool {
            if (text.empty() || !is_lower(text.front()) || text.back() == '_') {
                return false;
            }

            for (auto i = 1uz; i < text.size(); i++) {
                const auto valid =
                    text[i] == '_' ? text[i - 1] != '_' : is_lower(text[i]) || is_digit(text[i]);
                if (!valid) {
                    return false;
                }
            }

            return true;
        }

        constexpr auto is_key(std::string_view text) -> bool {
            return !text.empty() &&
                   std::ranges::all_of(std::views::split(text, '.'), [](auto segment) -> bool {
                       return is_segment(std::string_view{segment});
                   });
        }

        auto prefix(const config::origin& origin) -> std::string {
            if (origin.source.empty()) {
                return {};
            }

            return origin.line == 0 ? std::format("{}: ", origin.source)
                                    : std::format("{}:{}: ", origin.source, origin.line);
        }
    } // namespace

    command::command(std::string_view name, const config::command_info& info)
        : m_name(name), m_help(info.help), m_version(info.version),
          m_env_prefix(info.env_prefix.empty() ? std::string{} : std::format("{}__", info.env_prefix)),
          m_on_warning(info.on_warning) {}

    auto command::_add(std::string_view key, std::unique_ptr<detail::binding> target,
                       const config::option_info& info, detail::entry_kind kind) -> void {
        const auto derived = !info.env && !m_env_prefix.empty();

        std::optional<std::string> long_name;
        if (!info.long_name.empty()) {
            long_name = info.long_name;
        } else if (kind == detail::entry_kind::option) {
            long_name = key;
        }

        std::optional<std::string> env_name;
        if (derived) {
            env_name = detail::env_name(m_env_prefix, key);
        } else if (info.env && !info.env->empty()) {
            env_name = *info.env;
        }

        std::string value_name{info.value_name};
        if (value_name.empty()) {
            value_name = std::ranges::to<std::string>(key.substr(key.rfind('.') + 1) |
                                                      std::views::transform(core::to_ascii_upper));
        }

        m_entries.push_back({
            .kind = kind,
            .key = std::string{key},
            .long_name = std::move(long_name),
            .short_name = info.short_name == 0 ? std::nullopt : std::optional{info.short_name},
            .value_name = std::move(value_name),
            .help = std::string{info.help},
            .env_name = std::move(env_name),
            .env_derived = derived,
            .required = info.required,
            .hidden = info.hidden,
            .target = std::move(target),
        });
    }

    auto command::subcommand(std::string_view name, std::string_view help) -> config::command& {
        auto& child = m_subcommands.emplace_back(name, config::command_info{.help = help});
        child.m_env_prefix = m_env_prefix;

        return child;
    }

    auto command::_validate(std::span<const std::reference_wrapper<const detail::entry>> inherited) const
        -> std::expected<void, rei::error> {
        std::vector<std::reference_wrapper<const detail::entry>> visible{inherited.begin(), inherited.end()};
        auto variadic = false;
        auto optional = false;

        for (auto i = 0uz; i < m_entries.size(); i++) {
            const auto& entry = m_entries[i];
            const auto earlier = std::span{m_entries}.first(i);

            if (!is_key(entry.key)) {
                return invalid_schema(std::format("invalid key '{}'", entry.key));
            }
            if (std::ranges::contains(visible, entry.key, &detail::entry::key)) {
                return invalid_schema(std::format("duplicate key '{}'", entry.key));
            }

            if (entry.env_name && !entry.env_derived) {
                if (!m_env_prefix.empty() && entry.env_name->starts_with(m_env_prefix)) {
                    return invalid_schema(
                        std::format("environment name '{}' starts with '{}'", *entry.env_name, m_env_prefix));
                }
                if (std::ranges::contains(visible, entry.env_name, &detail::entry::env_name)) {
                    return invalid_schema(std::format("duplicate environment name '{}'", *entry.env_name));
                }
            }

            visible.emplace_back(entry);

            if (entry.kind == detail::entry_kind::argument) {
                if (entry.long_name || entry.short_name) {
                    return invalid_schema(std::format("argument '{}' has an option name", entry.key));
                }
                if (!m_subcommands.empty()) {
                    return invalid_schema(
                        std::format("argument '{}' is on a command with subcommands", entry.key));
                }
                if (variadic) {
                    return invalid_schema(std::format("argument '{}' follows a variadic one", entry.key));
                }
                if (entry.required && optional) {
                    return invalid_schema(
                        std::format("required argument '{}' follows an optional one", entry.key));
                }

                variadic = entry.target->kind() == detail::binding_kind::list;
                optional = !entry.required;
                continue;
            }

            if (!is_key(*entry.long_name)) {
                return invalid_schema(std::format("invalid option name '--{}'", *entry.long_name));
            }
            if (std::ranges::contains(earlier, entry.long_name, &detail::entry::long_name)) {
                return invalid_schema(std::format("duplicate option name '--{}'", *entry.long_name));
            }

            if (!entry.short_name) {
                continue;
            }

            const auto name = *entry.short_name;
            if (!is_lower(name) && !is_upper(name) && !is_digit(name)) {
                return invalid_schema(std::format("invalid short name for '{}'", entry.key));
            }
            if (std::ranges::contains(earlier, entry.short_name, &detail::entry::short_name)) {
                return invalid_schema(std::format("duplicate option name '-{}'", name));
            }
        }

        for (auto child = m_subcommands.begin(); child != m_subcommands.end(); child++) {
            if (!is_segment(child->m_name)) {
                return invalid_schema(std::format("invalid command name '{}'", child->m_name));
            }
            if (std::ranges::contains(m_subcommands.begin(), child, child->m_name, &command::m_name)) {
                return invalid_schema(std::format("duplicate command '{}'", child->m_name));
            }
            if (auto valid = child->_validate(visible); !valid) {
                return valid;
            }
        }

        return {};
    }

    auto command::load(int argc, const char* const* argv) -> std::expected<void, rei::error> {
        auto args = read_args(argc, argv);
        if (!args) {
            return std::unexpected(std::move(args).error());
        }

        auto env = read_env();
        if (!env) {
            return std::unexpected(std::move(env).error());
        }

        return apply(std::array{*std::move(env), *std::move(args)});
    }

    auto command::apply(std::span<const config::layer> layers) -> std::expected<void, rei::error> {
        if (auto valid = _validate(); !valid) {
            return valid;
        }

        for (const auto& layer : layers) {
            std::vector<std::string_view> unknown;

            for (const auto& value : layer) {
                if (std::ranges::contains(m_entries, value.key, &detail::entry::key) ||
                    std::ranges::contains(unknown, value.key)) {
                    continue;
                }

                unknown.push_back(value.key);

                const auto keys = std::ranges::to<std::vector<std::string>>(
                    m_entries | std::views::transform(&detail::entry::key));
                const auto warning = rei::error{
                    error_code::unknown_key, std::format("{}unknown key '{}'{}", prefix(value.origin),
                                                         value.key, detail::did_you_mean(value.key, keys))};

                if (m_on_warning) {
                    m_on_warning(warning);
                } else {
                    std::println(stderr, "warning: {}", warning.detail());
                }
            }
        }

        for (const auto& entry : m_entries) {
            std::vector<std::reference_wrapper<const config::value>> winning;

            for (const auto& layer : layers | std::views::reverse) {
                for (const auto& value : layer) {
                    if (value.key == entry.key) {
                        winning.emplace_back(value);
                    }
                }

                if (!winning.empty()) {
                    break;
                }
            }

            if (winning.empty()) {
                if (!entry.required) {
                    continue;
                }

                return std::unexpected(
                    rei::error{error_code::missing_required,
                               entry.kind == detail::entry_kind::argument
                                   ? std::format("missing required argument <{}>", entry.value_name)
                                   : std::format("missing required option '--{}'", *entry.long_name)});
            }

            const auto texts = std::ranges::to<std::vector<std::string_view>>(
                winning | std::views::transform(&config::value::text));

            if (auto assigned = entry.target->assign(texts); !assigned) {
                const auto& rejected = winning[assigned.error().index].get();
                const auto named = rejected.origin.line == 0 && !rejected.origin.source.empty();

                return std::unexpected(rei::error{error_code::invalid_value,
                                                  std::format("{}{}{}", prefix(rejected.origin),
                                                              named ? "" : std::format("{}: ", entry.key),
                                                              assigned.error().reason)});
            }
        }

        return {};
    }
} // namespace rei::config
