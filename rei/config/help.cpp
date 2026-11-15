#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <format>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/config/config.hpp"
#include "rei/config/detail.hpp"
#include "rei/config/error_code.hpp"
#include "rei/core/error.hpp"

namespace rei::config {
    namespace {
        constexpr auto line_width = 100uz;
        constexpr auto widest_help_column = 40uz;
        constexpr auto farthest_suggestion = 2uz;

        struct row {
            std::string label;
            std::vector<std::string> words;
        };

        auto edit_distance(std::string_view lhs, std::string_view rhs) -> std::size_t {
            std::vector<std::size_t> before(rhs.size() + 1);
            std::vector<std::size_t> previous(rhs.size() + 1);
            std::vector<std::size_t> current(rhs.size() + 1);

            for (auto j = 0uz; j <= rhs.size(); j++) {
                previous[j] = j;
            }

            for (auto i = 1uz; i <= lhs.size(); i++) {
                current[0] = i;

                for (auto j = 1uz; j <= rhs.size(); j++) {
                    const auto substitution = previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0 : 1);
                    current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});

                    if (i > 1 && j > 1 && lhs[i - 1] == rhs[j - 2] && lhs[i - 2] == rhs[j - 1]) {
                        current[j] = std::min(current[j], before[j - 2] + 1);
                    }
                }

                std::swap(before, previous);
                std::swap(previous, current);
            }

            return previous[rhs.size()];
        }

        auto placeholder(const detail::entry& entry) -> std::string {
            const auto* const rest = entry.target->kind() == detail::binding_kind::list ? "..." : "";
            return entry.required ? std::format("<{}>{}", entry.value_name, rest)
                                  : std::format("[{}]{}", entry.value_name, rest);
        }

        auto option_label(std::optional<char> short_name, std::optional<std::string_view> long_name)
            -> std::string {
            auto label = short_name ? std::format("  -{}", *short_name) : std::string(4, ' ');

            if (long_name) {
                label += short_name ? ", --" : "  --";
                label += *long_name;
            }

            return label;
        }

        auto split_words(std::string_view text) -> std::vector<std::string> {
            std::vector<std::string> words;

            for (const auto word : std::views::split(text, ' ')) {
                if (!word.empty()) {
                    words.emplace_back(std::string_view{word});
                }
            }

            return words;
        }

        auto describe(const detail::entry& entry) -> std::vector<std::string> {
            auto words = split_words(entry.help);

            if (const auto shown = entry.target->shown_default(); !shown.empty() && !entry.required) {
                words.push_back(std::format("[default: {}]", shown));
            }
            if (const auto choices = entry.target->choices(); !choices.empty()) {
                words.push_back(std::format("[possible values: {}]", detail::join(choices, ", ")));
            }

            return words;
        }

        auto write_rows(std::string& out, std::span<const row> rows) -> void {
            auto column = 0uz;
            for (const auto& row : rows) {
                if (row.label.size() + 2 <= widest_help_column) {
                    column = std::max(column, row.label.size() + 2);
                }
            }
            if (column == 0) {
                column = widest_help_column;
            }

            for (const auto& row : rows) {
                out += row.label;
                auto used = row.label.size();
                auto started = false;

                if (!row.words.empty() && used + 2 > column) {
                    out += '\n';
                    used = 0;
                }

                for (const auto& word : row.words) {
                    if (started && used + 1 + word.size() > line_width) {
                        out += '\n';
                        used = 0;
                        started = false;
                    }

                    out.append(started ? 1 : column - used, ' ');
                    out += word;
                    used = (started ? used + 1 : column) + word.size();
                    started = true;
                }

                out += '\n';
            }
        }
    } // namespace

    auto detail::join(std::span<const std::string_view> items, std::string_view separator) -> std::string {
        std::string joined;

        for (auto i = 0uz; i < items.size(); i++) {
            joined += i == 0 ? "" : separator;
            joined += items[i];
        }

        return joined;
    }

    auto detail::did_you_mean(std::string_view name, std::span<const std::string> candidates) -> std::string {
        std::optional<std::string_view> closest;
        auto least = farthest_suggestion + 1;

        for (const auto& candidate : candidates) {
            if (const auto distance = edit_distance(name, candidate); distance < least) {
                least = distance;
                closest = candidate;
            }
        }

        return closest ? std::format("; did you mean '{}'?", *closest) : std::string{};
    }

    auto command::_builtins() const -> std::vector<detail::builtin> {
        const auto free_long = [&](std::string_view name) -> std::optional<std::string_view> {
            const auto taken = std::ranges::any_of(
                m_entries, [&](const detail::entry& entry) -> bool { return entry.long_name == name; });
            return taken ? std::nullopt : std::optional{name};
        };
        const auto free_short = [&](char name) -> std::optional<char> {
            const auto taken = std::ranges::any_of(
                m_entries, [&](const detail::entry& entry) -> bool { return entry.short_name == name; });
            return taken ? std::nullopt : std::optional{name};
        };

        std::vector<detail::builtin> builtins;
        builtins.push_back({
            .long_name = free_long("help"),
            .short_name = free_short('h'),
            .help = "Print help",
            .code = error_code::help_requested,
        });

        if (!m_version.empty()) {
            builtins.push_back({
                .long_name = free_long("version"),
                .short_name = free_short('V'),
                .help = "Print version",
                .code = error_code::version_requested,
            });
        }

        std::erase_if(builtins, [](const detail::builtin& builtin) -> bool {
            return !builtin.long_name && !builtin.short_name;
        });

        return builtins;
    }

    auto command::_usage() const -> std::string {
        auto usage = std::format("Usage: {} [OPTIONS]", m_name);

        for (const auto& entry : m_entries) {
            if (entry.kind == detail::entry_kind::argument && !entry.hidden) {
                usage += ' ';
                usage += placeholder(entry);
            }
        }

        return usage;
    }

    auto command::help() const -> std::string {
        std::vector<row> arguments;
        std::vector<row> options;

        for (const auto& entry : m_entries) {
            if (entry.hidden) {
                continue;
            }

            if (entry.kind == detail::entry_kind::argument) {
                arguments.push_back(
                    {.label = std::format("  {}", placeholder(entry)), .words = describe(entry)});
                continue;
            }

            auto label = option_label(entry.short_name, entry.long_name);
            if (entry.target->kind() != detail::binding_kind::flag) {
                label += std::format(" <{}>", entry.value_name);
            }

            options.push_back({.label = std::move(label), .words = describe(entry)});
        }

        for (const auto& builtin : _builtins()) {
            options.push_back({
                .label = option_label(builtin.short_name, builtin.long_name),
                .words = split_words(builtin.help),
            });
        }

        auto text = m_help.empty() ? std::string{} : std::format("{}\n\n", m_help);
        text += _usage();
        text += '\n';

        if (!arguments.empty()) {
            text += "\nArguments:\n";
            write_rows(text, arguments);
        }
        if (!options.empty()) {
            text += "\nOptions:\n";
            write_rows(text, options);
        }

        return text;
    }

    auto command::report(const rei::error& error) const -> int {
        if (error.code() == error_code::help_requested) {
            std::print("{}", help());
            return 0;
        }
        if (error.code() == error_code::version_requested) {
            std::println("{} {}", m_name, m_version);
            return 0;
        }

        std::println(stderr, "error: {}\n\n{}",
                     error.detail().empty() ? error.code().message() : error.detail(), _usage());

        const auto builtins = _builtins();
        const auto help = std::ranges::find(builtins, error_code::help_requested, &detail::builtin::code);

        if (help != builtins.end()) {
            std::println(stderr, "\nFor more information, try '{}'.",
                         help->long_name ? "--help" : std::format("-{}", *help->short_name));
        }

        return 2;
    }
} // namespace rei::config
