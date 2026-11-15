#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rei::formats {
    template <typename Value> class dictionary {
      public:
        struct entry {
            std::string key;
            Value value;

            entry(std::string name, Value held) : key(std::move(name)), value(std::move(held)) {}
        };

      private:
        std::vector<dictionary::entry> m_entries;

      public:
        dictionary() = default;

        dictionary(std::initializer_list<dictionary::entry> entries) {
            for (const auto& [key, value] : entries) {
                set(key, value);
            }
        }

        [[nodiscard]] auto find(std::string_view key) const noexcept
            -> std::optional<std::reference_wrapper<const Value>> {
            const auto found = std::ranges::find(m_entries, key, &entry::key);
            return found == m_entries.end() ? std::nullopt : std::optional{std::cref(found->value)};
        }

        [[nodiscard]] auto find(std::string_view key) noexcept
            -> std::optional<std::reference_wrapper<Value>> {
            const auto found = std::ranges::find(m_entries, key, &entry::key);
            return found == m_entries.end() ? std::nullopt : std::optional{std::ref(found->value)};
        }

        template <std::convertible_to<std::string_view>... Keys>
        [[nodiscard]] auto find(std::string_view key, std::string_view next,
                                const Keys&... rest) const noexcept
            -> std::optional<std::reference_wrapper<const Value>> {
            return find(key).and_then(
                [&](const Value& child) -> std::optional<std::reference_wrapper<const Value>> {
                    return child.find(next, rest...);
                });
        }

        [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool {
            return std::ranges::contains(m_entries, key, &entry::key);
        }

        auto set(std::string_view key, Value value) -> Value& {
            const auto found = std::ranges::find(m_entries, key, &entry::key);
            if (found != m_entries.end()) {
                found->value = std::move(value);
                return found->value;
            }

            return m_entries.emplace_back(std::string{key}, std::move(value)).value;
        }

        auto remove(std::string_view key) -> bool {
            return std::erase_if(m_entries,
                                 [&](const entry& candidate) -> bool { return candidate.key == key; }) != 0;
        }

        [[nodiscard]] auto size() const noexcept -> std::size_t { return m_entries.size(); }
        [[nodiscard]] auto empty() const noexcept -> bool { return m_entries.empty(); }

        [[nodiscard]] auto begin() const noexcept -> std::vector<dictionary::entry>::const_iterator {
            return m_entries.begin();
        }

        [[nodiscard]] auto end() const noexcept -> std::vector<dictionary::entry>::const_iterator {
            return m_entries.end();
        }

        auto operator==(const dictionary& other) const -> bool {
            return size() == other.size() && std::ranges::all_of(m_entries, [&](const entry& own) -> bool {
                       const auto match = other.find(own.key);
                       return match && match->get() == own.value;
                   });
        }
    };
} // namespace rei::formats
