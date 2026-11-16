#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
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
        static constexpr auto indexed_from = 16uz;

        std::vector<dictionary::entry> m_entries;
        std::unique_ptr<std::vector<std::size_t>> m_slots;

        [[nodiscard]] auto _locate(std::string_view key) const noexcept -> std::optional<std::size_t> {
            if (!m_slots) {
                const auto found = std::ranges::find(m_entries, key, &entry::key);
                return found == m_entries.end()
                           ? std::nullopt
                           : std::optional{static_cast<std::size_t>(found - m_entries.begin())};
            }

            const auto& slots = *m_slots;
            const auto mask = slots.size() - 1;

            for (auto slot = std::hash<std::string_view>{}(key)&mask;; slot = (slot + 1) & mask) {
                if (slots[slot] == 0) {
                    return std::nullopt;
                }
                if (m_entries[slots[slot] - 1].key == key) {
                    return slots[slot] - 1;
                }
            }
        }

        auto _place(std::size_t position) noexcept -> void {
            auto& slots = *m_slots;
            const auto mask = slots.size() - 1;

            auto slot = std::hash<std::string_view>{}(m_entries[position].key) & mask;
            while (slots[slot] != 0) {
                slot = (slot + 1) & mask;
            }

            slots[slot] = position + 1;
        }

        auto _reindex() -> void {
            if (m_entries.size() < indexed_from) {
                m_slots.reset();
                return;
            }

            if (!m_slots) {
                m_slots = std::make_unique<std::vector<std::size_t>>();
            }

            m_slots->assign(std::bit_ceil(m_entries.size() * 2), 0);
            for (auto i = 0uz; i < m_entries.size(); i++) {
                _place(i);
            }
        }

      public:
        dictionary() = default;

        dictionary(std::initializer_list<dictionary::entry> entries) {
            for (const auto& [key, value] : entries) {
                set(key, value);
            }
        }

        dictionary(const dictionary& other)
            : m_entries(other.m_entries),
              m_slots(other.m_slots ? std::make_unique<std::vector<std::size_t>>(*other.m_slots) : nullptr) {}

        dictionary(dictionary&&) noexcept = default;
        ~dictionary() = default;

        auto operator=(const dictionary& other) -> dictionary& {
            if (this != &other) {
                *this = dictionary{other};
            }

            return *this;
        }

        auto operator=(dictionary&&) noexcept -> dictionary& = default;

        [[nodiscard]] auto find(std::string_view key) const noexcept
            -> std::optional<std::reference_wrapper<const Value>> {
            const auto position = _locate(key);
            return position ? std::optional{std::cref(m_entries[*position].value)} : std::nullopt;
        }

        [[nodiscard]] auto find(std::string_view key) noexcept
            -> std::optional<std::reference_wrapper<Value>> {
            const auto position = _locate(key);
            return position ? std::optional{std::ref(m_entries[*position].value)} : std::nullopt;
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
            return _locate(key).has_value();
        }

        auto set(std::string_view key, Value value) -> Value& {
            if (const auto position = _locate(key)) {
                auto& held = m_entries[*position].value;
                held = std::move(value);
                return held;
            }

            m_entries.emplace_back(std::string{key}, std::move(value));
            if (m_slots && m_entries.size() * 2 <= m_slots->size()) {
                _place(m_entries.size() - 1);
            } else {
                _reindex();
            }

            return m_entries.back().value;
        }

        auto remove(std::string_view key) -> bool {
            const auto position = _locate(key);
            if (!position) {
                return false;
            }

            m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(*position));
            _reindex();
            return true;
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
