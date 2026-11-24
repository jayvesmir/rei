#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string_view>

#include "rei/formats/json.hpp"

namespace rei::formats::json::detail {
    inline constexpr std::uint64_t doomed_bit = 0x40;
    inline constexpr std::uint64_t loose_bit = 0x80;
    inline constexpr std::uint64_t tethered_bit = 0x10;
    inline constexpr auto text_padding = 16uz;

    struct chunk {
        chunk* next;
    };

    struct arena {
        chunk* chunks;
        std::size_t slots;
        std::size_t text;
        bool dirty;
    };

    struct header {
        arena* owned;
        std::size_t capacity;
    };

    static_assert(sizeof(header) == sizeof(node));

    struct region {
        arena* owner;
        char* low;
        char* high;
    };

    struct access {
        static auto wrap(node held) noexcept -> json::value { return json::value{held}; }
        static auto peek(const json::value& held) noexcept -> const node& { return held.m_plain.node; }
        static auto held(json::value& value) noexcept -> node& { return value._held(); }
    };

    inline auto slots_of(const node& held) noexcept -> node* {
        return reinterpret_cast<node*>(held.slots);
    }

    inline auto slot_count(const node& held) noexcept -> std::size_t {
        return kind_of(held) == kind::object ? size_of(held) * 2 : size_of(held);
    }

    inline auto is_owner(const node& held) noexcept -> bool {
        return (held.meta & owner_bit) != 0;
    }

    inline auto is_loose(const node& held) noexcept -> bool {
        return (held.meta & loose_bit) != 0;
    }

    inline auto header_of(const node& held) noexcept -> header& {
        return *reinterpret_cast<header*>(slots_of(held) - 1);
    }

    [[nodiscard]] inline auto allocate(std::size_t bytes) -> void* {
        return ::operator new(bytes);
    }

    inline auto deallocate(void* block) noexcept -> void {
        ::operator delete(block);
    }

    [[nodiscard]] auto open_arena(std::size_t bytes) -> region;
    [[nodiscard]] auto extend_arena(arena& owner, std::size_t bytes) -> region;
    auto close_arena(arena& owner) noexcept -> void;

    [[nodiscard]] auto locate(const node& held, std::string_view key) noexcept -> std::optional<std::size_t>;
} // namespace rei::formats::json::detail
