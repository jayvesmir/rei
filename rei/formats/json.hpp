#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "rei/core/error.hpp"

namespace rei::formats::json {
    enum class kind : std::uint8_t {
        null,
        boolean,
        integer,
        floating,
        string,
        array,
        object,
    };

    class value;
    class array;
    class object;

    struct parse_options {
        std::size_t max_depth{128};
    };

    namespace detail {
        static_assert(std::endian::native == std::endian::little);

        template <typename T>
        concept text = std::convertible_to<const T&, std::string_view>;

        template <typename T, typename... Others>
        concept one_of = (std::same_as<T, Others> || ...);

        template <typename T>
        concept integer =
            std::integral<T> && !detail::one_of<T, bool, char, wchar_t, char8_t, char16_t, char32_t>;

        inline constexpr std::uint64_t kind_bits = 0x07;
        inline constexpr std::uint64_t owner_bit = 0x08;
        inline constexpr std::uint64_t inline_bit = 0x10;
        inline constexpr std::uint64_t unsigned_bit = 0x10;
        inline constexpr std::uint64_t tag_bits = 0xFF;
        inline constexpr auto size_shift = 8;
        inline constexpr auto inline_offset = 2uz;
        inline constexpr auto inline_capacity = 14uz;

        struct node {
            std::uint64_t meta;
            union {
                std::uint64_t natural;
                std::int64_t integer;
                double floating;
                const char* text;
                json::value* slots;
            };
        };

        struct plain {
            detail::node node;
        };

        struct access;

        constexpr auto tag(json::kind kind, std::uint64_t flags = 0, std::size_t size = 0) noexcept
            -> std::uint64_t {
            return std::to_underlying(kind) | flags | (static_cast<std::uint64_t>(size) << size_shift);
        }

        constexpr auto kind_of(const detail::node& held) noexcept -> json::kind {
            return static_cast<json::kind>(held.meta & kind_bits);
        }

        constexpr auto size_of(const detail::node& held) noexcept -> std::size_t {
            return static_cast<std::size_t>(held.meta >> size_shift);
        }

        template <detail::integer Integer>
        constexpr auto integer_node(Integer number) noexcept -> detail::node {
            if (std::in_range<std::int64_t>(number)) {
                return {.meta = tag(kind::integer), .integer = static_cast<std::int64_t>(number)};
            }

            return {.meta = tag(kind::integer, unsigned_bit), .natural = static_cast<std::uint64_t>(number)};
        }

        inline auto text_of(const detail::node& held) noexcept -> std::string_view {
            if ((held.meta & inline_bit) != 0) {
                return {reinterpret_cast<const char*>(&held) + inline_offset, size_of(held) & 0xFF};
            }

            return {held.text, size_of(held)};
        }

        [[nodiscard]] auto text_node(std::string_view text) -> detail::node;
        [[nodiscard]] auto clone(const detail::node& held) -> detail::node;
        [[nodiscard]] auto take(detail::node& held) -> detail::node;
        auto release(detail::node& held) noexcept -> void;
        auto touch(detail::node& held) noexcept -> void;
        [[nodiscard]] auto equal(const detail::node& lhs, const detail::node& rhs) noexcept -> bool;
    } // namespace detail

    class array {
      private:
        friend class value;
        friend struct detail::access;

        detail::node m_node;

        explicit array(detail::node held) noexcept : m_node(held) {}

      public:
        array() noexcept : m_node{.meta = detail::tag(kind::array), .natural = 0} {}
        array(std::initializer_list<json::value> items);
        array(const array& other) : m_node(detail::clone(other.m_node)) {}
        array(array&& other) noexcept : m_node(detail::take(other.m_node)) {}
        ~array() { detail::release(m_node); }

        auto operator=(const array& other) -> array&;
        auto operator=(array&& other) noexcept -> array&;

        [[nodiscard]] auto size() const noexcept -> std::size_t { return detail::size_of(m_node); }
        [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

        [[nodiscard]] auto operator[](std::size_t index) const noexcept -> const json::value&;
        [[nodiscard]] auto operator[](std::size_t index) noexcept -> json::value&;

        [[nodiscard]] auto begin() const noexcept -> std::span<const json::value>::iterator;
        [[nodiscard]] auto end() const noexcept -> std::span<const json::value>::iterator;
        [[nodiscard]] auto begin() noexcept -> std::span<json::value>::iterator;
        [[nodiscard]] auto end() noexcept -> std::span<json::value>::iterator;

        auto push_back(json::value item) -> json::value&;
        auto insert(std::size_t index, json::value item) -> json::value&;
        auto erase(std::size_t index) noexcept -> void;
        auto clear() noexcept -> void;
        auto reserve(std::size_t capacity) -> void;

        auto operator==(const array& other) const noexcept -> bool {
            return detail::equal(m_node, other.m_node);
        }
    };

    class object {
      private:
        friend class value;
        friend struct detail::access;

        detail::node m_node;

        explicit object(detail::node held) noexcept : m_node(held) {}

      public:
        struct entry;
        struct member;
        class iterator;

        object() noexcept : m_node{.meta = detail::tag(kind::object), .natural = 0} {}
        object(std::initializer_list<object::entry> entries);
        object(const object& other) : m_node(detail::clone(other.m_node)) {}
        object(object&& other) noexcept : m_node(detail::take(other.m_node)) {}
        ~object() { detail::release(m_node); }

        auto operator=(const object& other) -> object&;
        auto operator=(object&& other) noexcept -> object&;

        [[nodiscard]] auto find(std::string_view key) const noexcept
            -> std::optional<std::reference_wrapper<const json::value>>;
        [[nodiscard]] auto find(std::string_view key) noexcept
            -> std::optional<std::reference_wrapper<json::value>>;

        template <std::convertible_to<std::string_view>... Keys>
        [[nodiscard]] auto find(std::string_view key, std::string_view next,
                                const Keys&... rest) const noexcept
            -> std::optional<std::reference_wrapper<const json::value>>;

        [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool {
            return find(key).has_value();
        }

        auto set(std::string_view key, json::value item) -> json::value&;
        auto remove(std::string_view key) noexcept -> bool;
        auto clear() noexcept -> void;
        auto reserve(std::size_t capacity) -> void;

        [[nodiscard]] auto size() const noexcept -> std::size_t { return detail::size_of(m_node); }
        [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

        [[nodiscard]] auto begin() const noexcept -> object::iterator;
        [[nodiscard]] auto end() const noexcept -> object::iterator;

        auto operator==(const object& other) const noexcept -> bool {
            return detail::equal(m_node, other.m_node);
        }
    };

    class value {
      private:
        friend class array;
        friend class object;
        friend struct detail::access;

        union {
            detail::plain m_plain;
            json::array m_array;
            json::object m_object;
        };

        explicit value(detail::node held) noexcept { _hold(held); }

        auto _hold(detail::node held) noexcept -> void {
            switch (detail::kind_of(held)) {
            case kind::array:
                ::new (static_cast<void*>(&m_array)) json::array{held};
                break;
            case kind::object:
                ::new (static_cast<void*>(&m_object)) json::object{held};
                break;
            default:
                ::new (static_cast<void*>(&m_plain)) detail::plain{held};
            }
        }

        [[nodiscard]] auto _held() noexcept -> detail::node& {
            switch (kind()) {
            case kind::array:
                return m_array.m_node;
            case kind::object:
                return m_object.m_node;
            default:
                return m_plain.node;
            }
        }

        [[nodiscard]] auto _owns() const noexcept -> bool { return kind() >= kind::string; }

      public:
        value() noexcept : m_plain{} {}
        value(std::nullptr_t) noexcept : m_plain{} {}

        template <std::same_as<bool> Boolean>
        value(Boolean flag) noexcept
            : m_plain{{.meta = detail::tag(kind::boolean), .natural = flag ? 1U : 0U}} {}

        template <detail::integer Integer>
        value(Integer number) noexcept : m_plain{detail::integer_node(number)} {}

        template <std::floating_point Float>
        value(Float number) noexcept
            : m_plain{{.meta = detail::tag(kind::floating), .floating = static_cast<double>(number)}} {}

        template <detail::text Text>
        value(const Text& text) : m_plain{detail::text_node(std::string_view{text})} {}

        value(json::array items) noexcept : m_array(std::move(items)) {}
        value(json::object members) noexcept : m_object(std::move(members)) {}

        value(const value& other) {
            _hold(other._owns() ? detail::clone(other.m_plain.node) : other.m_plain.node);
        }
        value(value&& other) noexcept {
            _hold(other._owns() ? detail::take(other._held()) : other.m_plain.node);
        }

        ~value() {
            if (_owns()) {
                detail::release(_held());
            }
        }

        auto operator=(const value& other) -> value&;
        auto operator=(value&& other) noexcept -> value&;

        [[nodiscard]] auto kind() const noexcept -> json::kind { return detail::kind_of(m_plain.node); }
        [[nodiscard]] auto is_null() const noexcept -> bool { return kind() == kind::null; }

        [[nodiscard]] auto as_boolean() const noexcept -> std::optional<bool> {
            return kind() == kind::boolean ? std::optional{m_plain.node.natural != 0} : std::nullopt;
        }

        [[nodiscard]] auto as_integer() const noexcept -> std::optional<std::int64_t> {
            const auto& held = m_plain.node;
            const auto fits = kind() == kind::integer && (held.meta & detail::unsigned_bit) == 0;
            return fits ? std::optional{held.integer} : std::nullopt;
        }

        [[nodiscard]] auto as_unsigned() const noexcept -> std::optional<std::uint64_t> {
            const auto& held = m_plain.node;
            const auto fits =
                kind() == kind::integer && ((held.meta & detail::unsigned_bit) != 0 || held.integer >= 0);
            return fits ? std::optional{held.natural} : std::nullopt;
        }

        [[nodiscard]] auto as_floating() const noexcept -> std::optional<double> {
            const auto& held = m_plain.node;
            if (kind() == kind::floating) {
                return held.floating;
            }
            if (kind() != kind::integer) {
                return std::nullopt;
            }

            return (held.meta & detail::unsigned_bit) != 0 ? static_cast<double>(held.natural)
                                                           : static_cast<double>(held.integer);
        }

        [[nodiscard]] auto as_string() const noexcept -> std::optional<std::string_view> {
            return kind() == kind::string ? std::optional{detail::text_of(m_plain.node)} : std::nullopt;
        }

        [[nodiscard]] auto as_array() const noexcept -> std::optional<std::span<const json::value>> {
            const auto& held = m_plain.node;
            return kind() == kind::array
                       ? std::optional{std::span<const json::value>{held.slots, detail::size_of(held)}}
                       : std::nullopt;
        }

        [[nodiscard]] auto as_object() const noexcept
            -> std::optional<std::reference_wrapper<const json::object>> {
            return kind() == kind::object ? std::optional{std::cref(m_object)} : std::nullopt;
        }

        [[nodiscard]] auto edit_array() noexcept -> std::optional<std::reference_wrapper<json::array>> {
            return kind() == kind::array ? std::optional{std::ref(m_array)} : std::nullopt;
        }

        [[nodiscard]] auto edit_object() noexcept -> std::optional<std::reference_wrapper<json::object>> {
            return kind() == kind::object ? std::optional{std::ref(m_object)} : std::nullopt;
        }

        template <std::convertible_to<std::string_view>... Keys>
        [[nodiscard]] auto find(std::string_view key, const Keys&... rest) const noexcept
            -> std::optional<std::reference_wrapper<const json::value>> {
            return as_object().and_then(
                [&](const json::object& members) -> std::optional<std::reference_wrapper<const json::value>> {
                    return members.find(key, rest...);
                });
        }

        auto operator==(const value& other) const noexcept -> bool {
            return detail::equal(m_plain.node, other.m_plain.node);
        }
    };

    static_assert(sizeof(json::value) == 16);

    struct object::entry {
        std::string_view key;
        json::value value;

        entry(std::string_view name, json::value held) noexcept : key(name), value(std::move(held)) {}
    };

    struct object::member {
        std::string_view key;
        const json::value& value;
    };

    class object::iterator {
      private:
        std::span<const json::value>::iterator m_slot;

      public:
        using difference_type = std::ptrdiff_t;
        using value_type = object::member;

        iterator() = default;
        explicit iterator(std::span<const json::value>::iterator slot) noexcept : m_slot(slot) {}

        [[nodiscard]] auto operator*() const noexcept -> object::member {
            return {.key = *m_slot->as_string(), .value = m_slot[1]};
        }

        auto operator++() noexcept -> iterator& {
            m_slot += 2;
            return *this;
        }

        auto operator++(int) noexcept -> iterator {
            const auto before = *this;
            m_slot += 2;
            return before;
        }

        auto operator==(const iterator& other) const noexcept -> bool = default;
    };

    inline auto array::operator[](std::size_t index) const noexcept -> const json::value& {
        return m_node.slots[index];
    }

    inline auto array::operator[](std::size_t index) noexcept -> json::value& {
        detail::touch(m_node);
        return m_node.slots[index];
    }

    inline auto array::begin() const noexcept -> std::span<const json::value>::iterator {
        return std::span<const json::value>{m_node.slots, size()}.begin();
    }

    inline auto array::end() const noexcept -> std::span<const json::value>::iterator {
        return std::span<const json::value>{m_node.slots, size()}.end();
    }

    inline auto array::begin() noexcept -> std::span<json::value>::iterator {
        detail::touch(m_node);
        return std::span<json::value>{m_node.slots, size()}.begin();
    }

    inline auto array::end() noexcept -> std::span<json::value>::iterator {
        return begin() + static_cast<std::ptrdiff_t>(size());
    }

    inline auto object::begin() const noexcept -> object::iterator {
        return object::iterator{std::span<const json::value>{m_node.slots, size() * 2}.begin()};
    }

    inline auto object::end() const noexcept -> object::iterator {
        return object::iterator{std::span<const json::value>{m_node.slots, size() * 2}.end()};
    }

    template <std::convertible_to<std::string_view>... Keys>
    auto object::find(std::string_view key, std::string_view next, const Keys&... rest) const noexcept
        -> std::optional<std::reference_wrapper<const json::value>> {
        return find(key).and_then(
            [&](const json::value& child) -> std::optional<std::reference_wrapper<const json::value>> {
                return child.find(next, rest...);
            });
    }

    auto parse(std::string_view text, const json::parse_options& options = {})
        -> std::expected<json::value, rei::error>;

    auto serialize(const json::value& value) -> std::expected<std::string, rei::error>;
} // namespace rei::formats::json
