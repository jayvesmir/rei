#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
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

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rei::config {
    namespace {
#if defined(_WIN32)
        auto from_code_page(std::string_view text) -> std::string {
            if (text.empty()) {
                return {};
            }

            const auto bytes = static_cast<int>(text.size());
            const auto units = MultiByteToWideChar(CP_ACP, 0, text.data(), bytes, nullptr, 0);

            std::wstring wide(static_cast<std::size_t>(units), L'\0');
            MultiByteToWideChar(CP_ACP, 0, text.data(), bytes, wide.data(), units);

            const auto size =
                WideCharToMultiByte(CP_UTF8, 0, wide.data(), units, nullptr, 0, nullptr, nullptr);

            std::string utf8(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), units, utf8.data(), size, nullptr, nullptr);

            return utf8;
        }
#endif

        class reader {
          private:
            std::span<const std::string_view> m_args;
            std::span<const detail::entry> m_entries;
            std::span<const detail::builtin> m_builtins;
            config::layer m_layer;
            std::size_t m_positionals{};
            bool m_options_ended{};

            auto _add(const detail::entry& entry, std::string_view text, std::string_view source) -> void {
                m_layer.push_back({
                    .key = entry.key,
                    .text = std::string{text},
                    .origin = {.source = std::string{source}},
                });
            }

            auto _add_next(const detail::entry& entry, std::string_view spelling)
                -> std::expected<void, rei::error> {
                if (m_args.empty()) {
                    return std::unexpected(rei::error{error_code::missing_value,
                                                      std::format("option '{}' requires a value", spelling)});
                }

                _add(entry, m_args.front(), spelling);
                m_args = m_args.subspan(1);
                return {};
            }

            [[nodiscard]] auto _unknown(std::string_view spelling) const -> std::unexpected<rei::error> {
                std::vector<std::string> known;

                for (const auto& entry : m_entries) {
                    if (!entry.long_name) {
                        continue;
                    }

                    known.push_back(std::format("--{}", *entry.long_name));
                    if (entry.target->kind() == detail::binding_kind::flag) {
                        known.push_back(std::format("--no-{}", *entry.long_name));
                    }
                }
                for (const auto& builtin : m_builtins) {
                    if (builtin.long_name) {
                        known.push_back(std::format("--{}", *builtin.long_name));
                    }
                }

                return std::unexpected(rei::error{
                    error_code::unknown_option,
                    std::format("unknown option '{}'{}", spelling, detail::did_you_mean(spelling, known))});
            }

            auto _long(std::string_view arg) -> std::expected<void, rei::error> {
                const auto equals = arg.find('=');
                const auto spelling = arg.substr(0, equals);
                const auto name = spelling.substr(2);
                const auto attached =
                    equals == std::string_view::npos ? std::nullopt : std::optional{arg.substr(equals + 1)};

                const auto named = [&](std::string_view long_name) -> auto {
                    return std::ranges::find_if(m_entries, [&](const detail::entry& candidate) -> bool {
                        return candidate.long_name == long_name;
                    });
                };

                if (const auto entry = named(name); entry != m_entries.end()) {
                    if (attached) {
                        _add(*entry, *attached, spelling);
                        return {};
                    }
                    if (entry->target->kind() == detail::binding_kind::flag) {
                        _add(*entry, "true", spelling);
                        return {};
                    }

                    return _add_next(*entry, spelling);
                }

                if (name.starts_with("no-")) {
                    const auto entry = named(name.substr(3));

                    if (entry != m_entries.end() && entry->target->kind() == detail::binding_kind::flag) {
                        if (attached) {
                            return std::unexpected(
                                rei::error{error_code::invalid_value,
                                           std::format("option '{}' doesn't take a value", spelling)});
                        }

                        _add(*entry, "false", spelling);
                        return {};
                    }
                }

                for (const auto& builtin : m_builtins) {
                    if (builtin.long_name == name) {
                        return std::unexpected(rei::error{builtin.code});
                    }
                }

                return _unknown(spelling);
            }

            auto _short(std::string_view arg) -> std::expected<void, rei::error> {
                for (auto i = 1uz; i < arg.size(); i++) {
                    const auto spelling = std::format("-{}", arg[i]);
                    const auto entry =
                        std::ranges::find_if(m_entries, [&](const detail::entry& candidate) -> bool {
                            return candidate.short_name == arg[i];
                        });

                    if (entry == m_entries.end()) {
                        for (const auto& builtin : m_builtins) {
                            if (builtin.short_name == arg[i]) {
                                return std::unexpected(rei::error{builtin.code});
                            }
                        }

                        return std::unexpected(rei::error{error_code::unknown_option,
                                                          std::format("unknown option '{}'", spelling)});
                    }

                    if (entry->target->kind() == detail::binding_kind::flag) {
                        _add(*entry, "true", spelling);
                        continue;
                    }

                    if (const auto rest = arg.substr(i + 1); !rest.empty()) {
                        _add(*entry, rest, spelling);
                        return {};
                    }

                    return _add_next(*entry, spelling);
                }

                return {};
            }

            auto _positional(std::string_view arg) -> std::expected<void, rei::error> {
                auto index = 0uz;

                for (const auto& entry : m_entries) {
                    if (entry.kind != detail::entry_kind::argument) {
                        continue;
                    }

                    if (index == m_positionals || entry.target->kind() == detail::binding_kind::list) {
                        _add(entry, arg, std::format("<{}>", entry.value_name));
                        m_positionals++;
                        return {};
                    }

                    index++;
                }

                return std::unexpected(rei::error{error_code::unexpected_argument,
                                                  std::format("unexpected argument '{}'", arg)});
            }

          public:
            explicit reader(std::span<const std::string_view> args) : m_args(args) {}

            auto read(std::span<const detail::entry> entries, std::span<const detail::builtin> builtins,
                      bool has_subcommands) -> std::expected<std::optional<std::string_view>, rei::error> {
                m_entries = entries;
                m_builtins = builtins;

                while (!m_args.empty()) {
                    const auto arg = m_args.front();
                    m_args = m_args.subspan(1);

                    std::expected<void, rei::error> read;
                    if (m_options_ended || arg.size() < 2 || !arg.starts_with('-')) {
                        if (has_subcommands) {
                            return arg;
                        }

                        read = _positional(arg);
                    } else if (arg == "--") {
                        m_options_ended = true;
                    } else if (arg.starts_with("--")) {
                        read = _long(arg);
                    } else {
                        read = _short(arg);
                    }

                    if (!read) {
                        return std::unexpected(std::move(read).error());
                    }
                }

                return std::nullopt;
            }

            [[nodiscard]] auto layer() && -> config::layer { return std::move(m_layer); }
        };
    } // namespace

    auto command::read_args(int argc, const char* const* argv) -> std::expected<config::layer, rei::error> {
#if defined(_WIN32)
        std::vector<std::string> converted;
        for (auto i = 1; i < argc; i++) {
            converted.push_back(from_code_page(argv[i]));
        }

        const std::vector<std::string_view> args(converted.begin(), converted.end());
#else
        std::vector<std::string_view> args;
        for (auto i = 1; i < argc; i++) {
            args.emplace_back(argv[i]);
        }
#endif

        return read_args(args);
    }

    auto command::read_args(std::span<const std::string_view> args)
        -> std::expected<config::layer, rei::error> {
        if (auto valid = _validate(); !valid) {
            return std::unexpected(std::move(valid).error());
        }

        _deselect();

        reader reading{args};
        std::reference_wrapper<config::command> current{*this};

        while (true) {
            auto& active = current.get();
            active.m_selected = true;

            const auto builtins = active._builtins();
            auto stopped = reading.read(active.m_entries, builtins, !active.m_subcommands.empty());
            if (!stopped) {
                return std::unexpected(std::move(stopped).error());
            }
            if (active.m_subcommands.empty()) {
                return std::move(reading).layer();
            }

            const auto name = *stopped;
            if (!name) {
                return std::unexpected(rei::error{error_code::missing_command, "missing command"});
            }

            const auto chosen = std::ranges::find(active.m_subcommands, *name, &command::m_name);
            if (chosen == active.m_subcommands.end()) {
                const auto known = std::ranges::to<std::vector<std::string>>(
                    active.m_subcommands | std::views::transform(&command::m_name));

                return std::unexpected(
                    rei::error{error_code::unknown_command, std::format("unknown command '{}'{}", *name,
                                                                        detail::did_you_mean(*name, known))});
            }

            current = *chosen;
        }
    }
} // namespace rei::config
