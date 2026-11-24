#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

#include "rei/formats/json/detail.hpp"
#include "rei/formats/json/powers.hpp"

namespace rei::formats::json::detail {
    struct wide {
        std::uint64_t high;
        std::uint64_t low;
    };

    inline auto multiply(std::uint64_t lhs, std::uint64_t rhs) noexcept -> wide {
#if defined(__SIZEOF_INT128__)
        __extension__ using product_type = unsigned __int128;
        const auto product = static_cast<product_type>(lhs) * rhs;
        return {
            .high = static_cast<std::uint64_t>(product >> 64),
            .low = static_cast<std::uint64_t>(product),
        };
#elif defined(_MSC_VER) && defined(_M_X64)
        wide product{};
        product.low = _umul128(lhs, rhs, &product.high);
        return product;
#else
        const auto low_low = (lhs & 0xFFFFFFFF) * (rhs & 0xFFFFFFFF);
        const auto high_low = (lhs >> 32) * (rhs & 0xFFFFFFFF);
        const auto low_high = (lhs & 0xFFFFFFFF) * (rhs >> 32);
        const auto high_high = (lhs >> 32) * (rhs >> 32);
        const auto middle = (low_low >> 32) + (high_low & 0xFFFFFFFF) + (low_high & 0xFFFFFFFF);

        return {.high = high_high + (high_low >> 32) + (low_high >> 32) + (middle >> 32),
                .low = (middle << 32) | (low_low & 0xFFFFFFFF)};
#endif
    }

    inline constexpr std::array<double, 23> exact_powers_of_ten{
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };

    inline constexpr auto mantissa_bits = 52;
    inline constexpr auto exponent_bias = 1023;
    inline constexpr auto infinite_exponent = 0x7FF;

    // Clinger's fast path and the Eisel-Lemire algorithm, adapted from fast_float's compute_float
    // (https://github.com/fastfloat/fast_float, Copyright (c) 2021 The fast_float authors, MIT license).
    REI_JSON_INLINE auto scale(std::uint64_t digits, std::int64_t exponent) noexcept -> double {
        if (digits <= (1ULL << 53) && exponent >= -22 && exponent <= 22) {
            const auto exact = static_cast<double>(digits);
            return exponent < 0 ? exact / exact_powers_of_ten[static_cast<std::size_t>(-exponent)]
                                : exact * exact_powers_of_ten[static_cast<std::size_t>(exponent)];
        }

        if (digits == 0 || exponent < smallest_power_of_five) {
            return 0.0;
        }
        if (exponent > largest_power_of_five) {
            return std::numeric_limits<double>::infinity();
        }

        const auto leading = std::countl_zero(digits);
        const auto normalized = digits << leading;
        const auto index = static_cast<std::size_t>(exponent - smallest_power_of_five) * 2;

        auto product = multiply(normalized, powers_of_five[index]);
        if ((product.high & 0x1FF) == 0x1FF) {
            const auto second = multiply(normalized, powers_of_five[index + 1]);
            product.low += second.high;
            if (second.high > product.low) {
                product.high++;
            }
        }

        const auto upper = static_cast<std::int32_t>(product.high >> 63);
        const auto shift = upper + 64 - mantissa_bits - 3;
        auto mantissa = product.high >> shift;
        auto power = static_cast<std::int32_t>((((152170 + 65536) * exponent) >> 16) + 63) + upper - leading +
                     exponent_bias;

        if (power <= 0) {
            if (-power + 1 >= 64) {
                return 0.0;
            }

            mantissa >>= -power + 1;
            mantissa += mantissa & 1;
            mantissa >>= 1;
            power = mantissa < (1ULL << mantissa_bits) ? 0 : 1;
            return std::bit_cast<double>(mantissa | (static_cast<std::uint64_t>(power) << mantissa_bits));
        }

        const auto halfway = product.low <= 1 && exponent >= -4 && exponent <= 23 && (mantissa & 3) == 1 &&
                             (mantissa << shift) == product.high;
        if (halfway) {
            mantissa &= ~1ULL;
        }

        mantissa += mantissa & 1;
        mantissa >>= 1;
        if (mantissa >= (2ULL << mantissa_bits)) {
            mantissa = 1ULL << mantissa_bits;
            power++;
        }

        mantissa &= ~(1ULL << mantissa_bits);
        if (power >= infinite_exponent) {
            return std::numeric_limits<double>::infinity();
        }

        return std::bit_cast<double>(mantissa | (static_cast<std::uint64_t>(power) << mantissa_bits));
    }

    inline auto load_eight(const char* from) noexcept -> std::uint64_t {
        std::uint64_t word{};
        std::memcpy(&word, from, sizeof(word));
        return word;
    }

    constexpr auto powers_of_ten() noexcept -> std::array<std::uint64_t, 20> {
        std::array<std::uint64_t, 20> table{};
        auto power = std::uint64_t{1};
        for (auto& entry : table) {
            entry = power;
            power *= 10;
        }

        return table;
    }

    constexpr auto pairs_of_digits() noexcept -> std::array<char, 200> {
        std::array<char, 200> table{};
        for (auto pair = 0uz; pair < 100; pair++) {
            table[pair * 2] = static_cast<char>('0' + (pair / 10));
            table[(pair * 2) + 1] = static_cast<char>('0' + (pair % 10));
        }

        return table;
    }

    inline constexpr auto small_powers_of_ten = powers_of_ten();
    inline constexpr auto digit_pairs = pairs_of_digits();

    inline auto write_natural(char* into, std::uint64_t value) noexcept -> char* {
        const auto guess = static_cast<std::size_t>((std::bit_width(value | 1) * 1233) >> 12);
        auto* const end = into + guess + ((value | 1) >= small_powers_of_ten[guess] ? 1 : 0);
        auto* cursor = end;

        while (value >= 100) {
            cursor -= 2;
            std::memcpy(cursor, &digit_pairs[(value % 100) * 2], 2);
            value /= 100;
        }

        if (value >= 10) {
            std::memcpy(cursor - 2, &digit_pairs[value * 2], 2);
        } else {
            cursor[-1] = static_cast<char>('0' + value);
        }

        return end;
    }

    constexpr auto digit_count(std::uint64_t word) noexcept -> std::int32_t {
        const auto others = ((word + 0x4646464646464646) | (word - 0x3030303030303030)) & 0x8080808080808080;
        return std::countr_zero(others) >> 3;
    }

    constexpr auto is_eight_digits(std::uint64_t word) noexcept -> bool {
        return ((word & 0xF0F0F0F0F0F0F0F0) | (((word + 0x0606060606060606) & 0xF0F0F0F0F0F0F0F0) >> 4)) ==
               0x3333333333333333;
    }

    constexpr auto eight_digits(std::uint64_t word) noexcept -> std::uint64_t {
        word = (word & 0x0F0F0F0F0F0F0F0F) * 2561 >> 8;
        word = (word & 0x00FF00FF00FF00FF) * 6553601 >> 16;
        return (word & 0x0000FFFF0000FFFF) * 42949672960001 >> 32;
    }

    constexpr auto leading_digits(std::uint64_t word, std::int32_t count) noexcept -> std::uint64_t {
        return eight_digits(word << (64 - (count * 8)));
    }
} // namespace rei::formats::json::detail
