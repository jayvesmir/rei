#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();

namespace rei::formats::json::detail::simd {
    namespace portable = hwy::HWY_NAMESPACE;

    inline constexpr auto width = 16uz;
    inline constexpr auto longest_indent = (width * 2) - 1;

    using lanes = portable::FixedTag<std::uint8_t, width>;

    struct probe {
        std::uint32_t quotes;
        std::uint32_t stops;

        [[nodiscard]] auto closes() const noexcept -> bool {
            return quotes != 0 && ((quotes - 1) & ~quotes & stops) == 0;
        }

        [[nodiscard]] auto reach() const noexcept -> std::size_t {
            return static_cast<std::size_t>(std::countr_zero(quotes | (1U << width)));
        }

        [[nodiscard]] auto plain() const noexcept -> std::size_t {
            return static_cast<std::size_t>(std::countr_zero(quotes | stops | (1U << width)));
        }

        [[nodiscard]] auto plain_until(std::size_t count) const noexcept -> bool {
            return ((quotes | stops) & ((1U << count) - 1)) == 0;
        }
    };

    HWY_ATTR inline auto load_block(const char* from) noexcept -> portable::VFromD<lanes> {
        return portable::LoadU(lanes{}, reinterpret_cast<const std::uint8_t*>(from));
    }

    HWY_ATTR inline auto move_block(const char* from, char* into) noexcept -> void {
        portable::StoreU(load_block(from), lanes{}, reinterpret_cast<std::uint8_t*>(into));
    }

    HWY_ATTR inline auto same_as(portable::VFromD<lanes> block, char unit) noexcept
        -> portable::MFromD<lanes> {
        return portable::Eq(block, portable::Set(lanes{}, static_cast<std::uint8_t>(unit)));
    }

    HWY_ATTR inline auto probe_text(const char* from, char* into) noexcept -> probe {
        const lanes tag;
        const portable::RebindToSigned<lanes> signed_tag;
        const auto block = load_block(from);
        portable::StoreU(block, tag, reinterpret_cast<std::uint8_t*>(into));

        const auto control =
            portable::Eq(portable::Min(block, portable::Set(tag, std::uint8_t{0x1F})), block);
        const auto wide =
            portable::RebindMask(tag, portable::IsNegative(portable::BitCast(signed_tag, block)));
        const auto stops = portable::Or(portable::Or(same_as(block, '\\'), control), wide);

        return {
            .quotes = static_cast<std::uint32_t>(portable::BitsFromMask(tag, same_as(block, '"'))),
            .stops = static_cast<std::uint32_t>(portable::BitsFromMask(tag, stops)),
        };
    }

    HWY_ATTR inline auto copy_plain(const char* from, char* into) noexcept -> std::size_t {
        return probe_text(from, into).plain();
    }

    HWY_ATTR inline auto space_bits(const char* from) noexcept -> std::uint64_t {
        return portable::BitsFromMask(lanes{}, same_as(load_block(from), ' '));
    }

    HWY_ATTR inline auto leading_spaces(const char* from) noexcept -> std::size_t {
        return static_cast<std::size_t>(std::countr_one(static_cast<std::uint16_t>(space_bits(from))));
    }

    HWY_ATTR inline auto is_indent(const char* from, std::size_t count) noexcept -> bool {
        auto spaces = space_bits(from);
        if (count >= width) {
            spaces |= space_bits(from + width) << width;
        }

        return (spaces & ((std::uint64_t{2} << count) - 1)) == (std::uint64_t{1} << count) - 1;
    }
} // namespace rei::formats::json::detail::simd

HWY_AFTER_NAMESPACE();
