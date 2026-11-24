#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string_view>

#include "rei/formats/json.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
#define REI_JSON_INLINE __forceinline
#define REI_JSON_OUTLINE __declspec(noinline)
#else
#define REI_JSON_INLINE [[gnu::always_inline]] inline
#define REI_JSON_OUTLINE [[gnu::noinline]]
#endif

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

    constexpr auto is_continuation(char unit) noexcept -> bool {
        return (static_cast<std::uint8_t>(unit) & 0xC0) == 0x80;
    }

    inline auto sequence_length(const char* from) noexcept -> std::size_t {
        std::uint32_t quad{};
        std::memcpy(&quad, from, sizeof(quad));

        if ((quad & 0x00C0C0F0) == 0x008080E0) {
            const auto range = quad & 0x0000200F;
            return range != 0 && range != 0x0000200D ? 3 : 0;
        }
        if ((quad & 0x0000C0E0) == 0x000080C0) {
            return (quad & 0x0000001E) != 0 ? 2 : 0;
        }
        if ((quad & 0xC0C0C0F8) == 0x808080F0) {
            const auto above = (quad & 0x00003007) != 0;
            const auto below = (quad & 0x00000004) == 0 || (quad & 0x00003003) == 0;
            return above && below ? 4 : 0;
        }

        return 0;
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
