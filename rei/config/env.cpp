#include <algorithm>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "rei/config/config.hpp"
#include "rei/config/detail.hpp"
#include "rei/core/ascii.hpp"
#include "rei/core/error.hpp"

namespace rei::config {
    namespace {
        auto key_of(std::string_view name) -> std::string {
            std::string key;

            while (!name.empty()) {
                const auto dot = name.starts_with("__");
                key += dot ? '.' : core::to_ascii_lower(name.front());
                name.remove_prefix(dot ? 2 : 1);
            }

            return key;
        }
    } // namespace

    auto detail::env_name(std::string_view prefix, std::string_view key) -> std::string {
        std::string name{prefix};

        for (const auto unit : key) {
            if (unit == '.') {
                name += "__";
            } else {
                name += core::to_ascii_upper(unit);
            }
        }

        return name;
    }

    auto command::read_env() const -> std::expected<config::layer, rei::error> {
        std::vector<std::string_view> environment;

#if defined(_WIN32)
        std::string block;

        if (const auto wide = GetEnvironmentStringsW(); wide != nullptr) {
            auto length = 0uz;
            while (wide[length] != L'\0') {
                length += std::wstring_view{wide + length}.size() + 1;
            }

            const auto units = static_cast<int>(length);
            const auto bytes = WideCharToMultiByte(CP_UTF8, 0, wide, units, nullptr, 0, nullptr, nullptr);

            block.resize(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(CP_UTF8, 0, wide, units, block.data(), bytes, nullptr, nullptr);

            FreeEnvironmentStringsW(wide);
        }

        for (const auto variable : std::views::split(block, '\0')) {
            environment.emplace_back(variable);
        }
#else
#if defined(__APPLE__)
        const char* const* const variables = *_NSGetEnviron();
#else
        const char* const* const variables = environ;
#endif

        for (auto i = 0uz; variables != nullptr && variables[i] != nullptr; i++) {
            environment.emplace_back(variables[i]);
        }
#endif

        return read_env(environment);
    }

    auto command::read_env(std::span<const std::string_view> environment) const
        -> std::expected<config::layer, rei::error> {
        if (auto valid = _validate(); !valid) {
            return std::unexpected(std::move(valid).error());
        }

        config::layer layer;

        for (const auto variable : environment) {
            const auto equals = variable.find('=');
            if (equals == std::string_view::npos || equals + 1 == variable.size()) {
                continue;
            }

            const auto name = variable.substr(0, equals);
            const auto text = variable.substr(equals + 1);
            const auto prefixed = !m_env_prefix.empty() && name.starts_with(m_env_prefix);
            const auto key = prefixed ? key_of(name.substr(m_env_prefix.size())) : std::string{};

            const auto add = [&](std::string_view owner, std::string_view item) -> void {
                layer.push_back({
                    .key = std::string{owner},
                    .text = std::string{item},
                    .origin = {.source = std::string{name}},
                });
            };

            const auto entry = std::ranges::find_if(m_entries, [&](const detail::entry& candidate) -> bool {
                return prefixed ? candidate.key == key : !candidate.env_derived && candidate.env_name == name;
            });

            if (entry == m_entries.end()) {
                if (prefixed) {
                    add(key, text);
                }
                continue;
            }
            if (prefixed && !entry->env_derived) {
                continue;
            }

            if (entry->target->kind() != detail::binding_kind::list) {
                add(entry->key, text);
                continue;
            }

            for (const auto item : std::views::split(text, ',')) {
                add(entry->key, std::string_view{item});
            }
        }

        return layer;
    }
} // namespace rei::config
