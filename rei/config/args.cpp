#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
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
        class reader {
          private:
            std::span<const detail::entry> m_entries;
            std::span<const detail::builtin> m_builtins;
            std::span<const std::string_view> m_args;
            config::layer m_layer;
            std::size_t m_positionals{};

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
            reader(std::span<const detail::entry> entries, std::span<const detail::builtin> builtins,
                   std::span<const std::string_view> args)
                : m_entries(entries), m_builtins(builtins), m_args(args) {}

            auto read() -> std::expected<config::layer, rei::error> {
                auto options_ended = false;

                while (!m_args.empty()) {
                    const auto arg = m_args.front();
                    m_args = m_args.subspan(1);

                    std::expected<void, rei::error> read;
                    if (options_ended || arg.size() < 2 || !arg.starts_with('-')) {
                        read = _positional(arg);
                    } else if (arg == "--") {
                        options_ended = true;
                    } else if (arg.starts_with("--")) {
                        read = _long(arg);
                    } else {
                        read = _short(arg);
                    }

                    if (!read) {
                        return std::unexpected(std::move(read).error());
                    }
                }

                return std::move(m_layer);
            }
        };
    } // namespace

    auto command::read_args(int argc, const char* const* argv) -> std::expected<config::layer, rei::error> {
        std::vector<std::string_view> args;
        for (auto i = 1; i < argc; i++) {
            args.emplace_back(argv[i]);
        }

        return read_args(args);
    }

    auto command::read_args(std::span<const std::string_view> args)
        -> std::expected<config::layer, rei::error> {
        if (auto valid = _validate(); !valid) {
            return std::unexpected(std::move(valid).error());
        }

        const auto builtins = _builtins();
        return reader{m_entries, builtins, args}.read();
    }
} // namespace rei::config
