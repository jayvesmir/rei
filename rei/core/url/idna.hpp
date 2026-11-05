#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "rei/core/error.hpp"

// UTS #46 processing for the URL standard's domain to ASCII. url/idna_tables.bzl generates the tables from
// Unicode data, sorted by code point.
namespace rei::core::detail::idna {
    // Deviations are `valid`, since only nontransitional processing is implemented.
    enum class status : std::uint8_t {
        valid,
        ignored,
        mapped,
        disallowed,
    };

    // The classes RFC 5893 tells apart, plus `other` for the rest. Code points without an entry are L.
    enum class bidi_class : std::uint8_t {
        left_to_right,
        right_to_left,
        arabic_letter,
        arabic_number,
        european_number,
        european_separator,
        european_terminator,
        common_separator,
        other_neutral,
        boundary_neutral,
        nonspacing_mark,
        other,
    };

    // Code points without an entry are non-joining.
    enum class joining_type : std::uint8_t {
        non_joining,
        join_causing,
        dual,
        left,
        right,
        transparent,
    };

    // Covers every code point. A mapped range maps to `length` code points at `offset` in
    // `mapped_code_points`.
    struct mapping {
        char32_t first;
        char32_t last;
        idna::status status;
        std::uint16_t offset;
        std::uint8_t length;
    };

    struct code_point_range {
        char32_t first;
        char32_t last;
    };

    template <typename Value> struct property {
        char32_t first;
        char32_t last;
        Value value;
    };

    // The full canonical decomposition, `length` code points at `offset` in `decomposed_code_points`.
    // Hangul syllables decompose algorithmically and have no entry.
    struct decomposition {
        char32_t code_point;
        std::uint16_t offset;
        std::uint8_t length;
    };

    // Sorted by `first`, then `second`. Composition exclusions and Hangul syllables have no entry.
    struct composition {
        char32_t first;
        char32_t second;
        char32_t composite;
    };

    extern const std::span<const idna::mapping> mappings;
    extern const std::span<const char32_t> mapped_code_points;
    extern const std::span<const idna::property<idna::bidi_class>> bidi_classes;
    extern const std::span<const idna::property<idna::joining_type>> joining_types;
    extern const std::span<const idna::property<std::uint8_t>> combining_classes; // Only nonzero classes
    extern const std::span<const idna::code_point_range> combining_marks; // General_Category M
    extern const std::span<const idna::decomposition> decompositions;
    extern const std::span<const char32_t> decomposed_code_points;
    extern const std::span<const idna::composition> compositions;

    // UTS #46 ToASCII with CheckBidi and CheckJoiners set and every other flag unset, as the URL standard's
    // non-strict domain to ASCII uses it. `domain` must be valid UTF-8.
    auto to_ascii(std::string_view domain) -> std::expected<std::string, rei::error>;
} // namespace rei::core::detail::idna
