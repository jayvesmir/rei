#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/core/error_code.hpp"
#include "rei/core/url/idna.hpp"

namespace rei::core::detail::idna {
    namespace {
        // Hangul syllable (de)composition, from chapter 3.12 of the Unicode standard
        constexpr char32_t hangul_s_base = 0xAC00;
        constexpr char32_t hangul_l_base = 0x1100;
        constexpr char32_t hangul_v_base = 0x1161;
        constexpr char32_t hangul_t_base = 0x11A7;
        constexpr char32_t hangul_l_count = 19;
        constexpr char32_t hangul_v_count = 21;
        constexpr char32_t hangul_t_count = 28;
        constexpr char32_t hangul_n_count = hangul_v_count * hangul_t_count;
        constexpr char32_t hangul_s_count = hangul_l_count * hangul_n_count;

        // Punycode parameters, from RFC 3492 section 5
        constexpr std::uint32_t punycode_base = 36;
        constexpr std::uint32_t punycode_t_min = 1;
        constexpr std::uint32_t punycode_t_max = 26;
        constexpr std::uint32_t punycode_skew = 38;
        constexpr std::uint32_t punycode_damp = 700;
        constexpr std::uint32_t punycode_initial_bias = 72;
        constexpr std::uint32_t punycode_initial_n = 0x80;
        constexpr std::uint32_t punycode_max = std::numeric_limits<std::uint32_t>::max();

        constexpr std::uint8_t virama_combining_class = 9;
        constexpr char32_t zero_width_non_joiner = 0x200C;
        constexpr char32_t zero_width_joiner = 0x200D;

        auto invalid(std::string detail, std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{core::make_error_code(core::error_code::invalid_argument),
                                              std::move(detail), location});
        }

        auto is_ascii(char32_t code_point) -> bool {
            return code_point < 0x80;
        }

        template <typename Range>
        auto find_range(std::span<const Range> table, char32_t code_point) -> const Range* {
            auto next = std::ranges::upper_bound(table, code_point, {}, &Range::first);
            if (next == table.begin() || std::prev(next)->last < code_point) {
                return nullptr;
            }

            return &*std::prev(next);
        }

        auto mapping_of(char32_t code_point) -> const idna::mapping& {
            return *idna::find_range(idna::mappings, code_point);
        }

        auto bidi_class_of(char32_t code_point) -> idna::bidi_class {
            const auto* entry = idna::find_range(idna::bidi_classes, code_point);
            return entry != nullptr ? entry->value : bidi_class::left_to_right;
        }

        auto joining_type_of(char32_t code_point) -> idna::joining_type {
            const auto* entry = idna::find_range(idna::joining_types, code_point);
            return entry != nullptr ? entry->value : joining_type::non_joining;
        }

        auto combining_class_of(char32_t code_point) -> std::uint8_t {
            const auto* entry = idna::find_range(idna::combining_classes, code_point);
            return entry != nullptr ? entry->value : 0;
        }

        auto decode_utf8(std::string_view text) -> std::u32string {
            std::u32string code_points;
            for (auto i = 0uz; i < text.size();) {
                auto lead = static_cast<std::uint8_t>(text[i]);
                auto ones = static_cast<std::size_t>(std::countl_one(lead));
                auto length = std::max(ones, 1uz);
                auto code_point = static_cast<char32_t>(lead & (0xFF >> (ones + 1)));
                for (auto j = 1uz; j < length; j++) {
                    code_point = (code_point << 6) | (static_cast<std::uint8_t>(text[i + j]) & 0x3F);
                }

                code_points += code_point;
                i += length;
            }

            return code_points;
        }

        // Disallowed code points stay, so that validation rejects them only if they survive normalization.
        auto map(std::u32string_view text) -> std::u32string {
            std::u32string mapped;
            for (auto code_point : text) {
                const auto& entry = idna::mapping_of(code_point);
                if (entry.status == status::mapped) {
                    auto values = idna::mapped_code_points.subspan(entry.offset, entry.length);
                    mapped.append(values.begin(), values.end());
                } else if (entry.status != status::ignored) {
                    mapped += code_point;
                }
            }

            return mapped;
        }

        auto decompose(std::u32string_view text) -> std::u32string {
            std::u32string decomposed;
            for (auto code_point : text) {
                if (code_point >= hangul_s_base && code_point < hangul_s_base + hangul_s_count) {
                    auto index = code_point - hangul_s_base;
                    decomposed += static_cast<char32_t>(hangul_l_base + (index / hangul_n_count));
                    decomposed +=
                        static_cast<char32_t>(hangul_v_base + (index % hangul_n_count / hangul_t_count));
                    if (index % hangul_t_count != 0) {
                        decomposed += static_cast<char32_t>(hangul_t_base + (index % hangul_t_count));
                    }

                    continue;
                }

                auto entry = std::ranges::lower_bound(idna::decompositions, code_point, {},
                                                      &decomposition::code_point);
                if (entry != idna::decompositions.end() && entry->code_point == code_point) {
                    auto parts = idna::decomposed_code_points.subspan(entry->offset, entry->length);
                    decomposed.append(parts.begin(), parts.end());
                } else {
                    decomposed += code_point;
                }
            }

            // Canonical ordering: a stable sort of every run of non-starters by combining class
            for (auto i = 1uz; i < decomposed.size(); i++) {
                auto combining_class = idna::combining_class_of(decomposed[i]);
                for (auto j = i; j > 0 && combining_class != 0 &&
                                 idna::combining_class_of(decomposed[j - 1]) > combining_class;
                     j--) {
                    std::swap(decomposed[j - 1], decomposed[j]);
                }
            }

            return decomposed;
        }

        auto compose_pair(char32_t first, char32_t second) -> std::optional<char32_t> {
            if (first >= hangul_l_base && first < hangul_l_base + hangul_l_count && second >= hangul_v_base &&
                second < hangul_v_base + hangul_v_count) {
                return static_cast<char32_t>(
                    hangul_s_base + ((((first - hangul_l_base) * hangul_v_count) + (second - hangul_v_base)) *
                                     hangul_t_count));
            }
            if (first >= hangul_s_base && first < hangul_s_base + hangul_s_count &&
                (first - hangul_s_base) % hangul_t_count == 0 && second > hangul_t_base &&
                second < hangul_t_base + hangul_t_count) {
                return static_cast<char32_t>(first + (second - hangul_t_base));
            }

            auto entry = std::ranges::lower_bound(idna::compositions, std::pair{first, second}, {},
                                                  [](const idna::composition& composition) -> auto {
                                                      return std::pair{composition.first, composition.second};
                                                  });
            if (entry != idna::compositions.end() && entry->first == first && entry->second == second) {
                return entry->composite;
            }

            return std::nullopt;
        }

        // NFC, following the canonical composition algorithm in UAX #15
        auto normalize(std::u32string_view text) -> std::u32string {
            auto result = idna::decompose(text);
            if (result.empty()) {
                return result;
            }

            // A character composes with the last starter unless one in between has the same or a higher
            // combining class, or is a starter itself. A leading non-starter can't compose with anything.
            auto starter = 0uz;
            auto size = 1uz;
            std::int32_t last_class = idna::combining_class_of(result.front()) == 0 ? 0 : 256;
            for (auto i = 1uz; i < result.size(); i++) {
                auto code_point = result[i];
                std::int32_t combining_class = idna::combining_class_of(code_point);
                auto composite = idna::compose_pair(result[starter], code_point);
                if (composite && (last_class < combining_class || last_class == 0)) {
                    result[starter] = *composite;
                    continue;
                }

                if (combining_class == 0) {
                    starter = size;
                }

                last_class = combining_class;
                result[size++] = code_point;
            }

            result.resize(size);
            return result;
        }

        // RFC 3492 calls `step` k.
        auto punycode_threshold(std::uint32_t step, std::uint32_t bias) -> std::uint32_t {
            if (step <= bias) {
                return punycode_t_min;
            }

            return step >= bias + punycode_t_max ? punycode_t_max : step - bias;
        }

        auto punycode_adapt(std::uint32_t delta, std::uint32_t points, bool first) -> std::uint32_t {
            delta = first ? delta / punycode_damp : delta / 2;
            delta += delta / points;

            std::uint32_t step = 0;
            while (delta > ((punycode_base - punycode_t_min) * punycode_t_max) / 2) {
                delta /= punycode_base - punycode_t_min;
                step += punycode_base;
            }

            return step + (((punycode_base - punycode_t_min + 1) * delta) / (delta + punycode_skew));
        }

        auto punycode_digit(char32_t unit) -> std::optional<std::uint32_t> {
            if (unit >= U'0' && unit <= U'9') {
                return unit - U'0' + 26;
            }
            if (unit >= U'a' && unit <= U'z') {
                return unit - U'a';
            }
            if (unit >= U'A' && unit <= U'Z') {
                return unit - U'A';
            }

            return std::nullopt;
        }

        auto punycode_digit_char(std::uint32_t digit) -> char {
            return static_cast<char>(digit < 26 ? 'a' + digit : '0' + (digit - 26));
        }

        // RFC 3492 section 6.2, for ASCII `input`. RFC 3492 calls `current` n and `index` i.
        auto punycode_decode(std::u32string_view input) -> std::optional<std::u32string> {
            // Without basic code points before it, the last delimiter is read as a digit, which fails.
            std::u32string output;
            if (auto delimiter = input.rfind(U'-'); delimiter != std::u32string_view::npos && delimiter > 0) {
                output = input.substr(0, delimiter);
                input.remove_prefix(delimiter + 1);
            }

            std::uint32_t current = punycode_initial_n;
            std::uint32_t index = 0;
            std::uint32_t bias = punycode_initial_bias;
            for (auto position = 0uz; position < input.size();) {
                auto old_index = index;
                std::uint32_t weight = 1;
                for (auto step = punycode_base;; step += punycode_base) {
                    if (position == input.size()) {
                        return std::nullopt;
                    }

                    auto digit = idna::punycode_digit(input[position]);
                    position++;
                    if (!digit || *digit > (punycode_max - index) / weight) {
                        return std::nullopt;
                    }

                    index += *digit * weight;
                    auto threshold = idna::punycode_threshold(step, bias);
                    if (*digit < threshold) {
                        break;
                    }
                    if (weight > punycode_max / (punycode_base - threshold)) {
                        return std::nullopt;
                    }

                    weight *= punycode_base - threshold;
                }

                auto length = static_cast<std::uint32_t>(output.size() + 1);
                bias = idna::punycode_adapt(index - old_index, length, old_index == 0);
                if (index / length > punycode_max - current) {
                    return std::nullopt;
                }

                current += index / length;
                index %= length;
                if (current > 0x10FFFF) {
                    return std::nullopt;
                }

                output.insert(index, 1, static_cast<char32_t>(current));
                index++;
            }

            return output;
        }

        // RFC 3492 section 6.3. RFC 3492 calls `current` n, `handled` h and `remaining` q.
        auto punycode_encode(std::u32string_view input) -> std::optional<std::string> {
            std::string output;
            for (auto code_point : input | std::views::filter(is_ascii)) {
                output += static_cast<char>(code_point);
            }

            auto basic = static_cast<std::uint32_t>(output.size());
            auto handled = basic;
            if (basic > 0) {
                output += '-';
            }

            std::uint32_t current = punycode_initial_n;
            std::uint32_t delta = 0;
            std::uint32_t bias = punycode_initial_bias;
            while (handled < input.size()) {
                auto next =
                    std::ranges::min(input | std::views::filter([current](char32_t code_point) -> bool {
                                         return code_point >= current;
                                     }));
                if (next - current > (punycode_max - delta) / (handled + 1)) {
                    return std::nullopt;
                }

                delta += (next - current) * (handled + 1);
                current = next;
                for (auto code_point : input) {
                    if (code_point < current) {
                        if (delta == punycode_max) {
                            return std::nullopt;
                        }

                        delta++;
                    }
                    if (code_point != current) {
                        continue;
                    }

                    auto remaining = delta;
                    for (auto step = punycode_base;; step += punycode_base) {
                        auto threshold = idna::punycode_threshold(step, bias);
                        if (remaining < threshold) {
                            break;
                        }

                        output += idna::punycode_digit_char(
                            threshold + ((remaining - threshold) % (punycode_base - threshold)));
                        remaining = (remaining - threshold) / (punycode_base - threshold);
                    }

                    output += idna::punycode_digit_char(remaining);
                    bias = idna::punycode_adapt(delta, handled + 1, handled == basic);
                    delta = 0;
                    handled++;
                }

                delta++;
                current++;
            }

            return output;
        }

        // RFC 5892 appendix A.1 and A.2
        auto satisfies_context_j(std::u32string_view label) -> bool {
            for (auto i = 0uz; i < label.size(); i++) {
                if (label[i] != zero_width_non_joiner && label[i] != zero_width_joiner) {
                    continue;
                }
                if (i > 0 && idna::combining_class_of(label[i - 1]) == virama_combining_class) {
                    continue;
                }
                if (label[i] == zero_width_joiner) {
                    return false;
                }

                // A ZERO WIDTH NON-JOINER needs (L|D) T* before it and T* (R|D) after it.
                auto before = i;
                while (before > 0 && idna::joining_type_of(label[before - 1]) == joining_type::transparent) {
                    before--;
                }

                auto after = i + 1;
                while (after < label.size() &&
                       idna::joining_type_of(label[after]) == joining_type::transparent) {
                    after++;
                }

                auto joins_before =
                    before > 0 && (idna::joining_type_of(label[before - 1]) == joining_type::left ||
                                   idna::joining_type_of(label[before - 1]) == joining_type::dual);
                auto joins_after =
                    after < label.size() && (idna::joining_type_of(label[after]) == joining_type::right ||
                                             idna::joining_type_of(label[after]) == joining_type::dual);
                if (!joins_before || !joins_after) {
                    return false;
                }
            }

            return true;
        }

        // RFC 5893 section 2. Empty labels have nothing to check.
        auto satisfies_bidi_rule(std::u32string_view label) -> bool {
            using enum idna::bidi_class;

            constexpr std::array rtl_classes{
                right_to_left,    arabic_letter,       arabic_number, european_number,  european_separator,
                common_separator, european_terminator, other_neutral, boundary_neutral, nonspacing_mark,
            };
            constexpr std::array rtl_ends{right_to_left, arabic_letter, european_number, arabic_number};
            constexpr std::array ltr_classes{
                left_to_right,       european_number, european_separator, common_separator,
                european_terminator, other_neutral,   boundary_neutral,   nonspacing_mark,
            };
            constexpr std::array ltr_ends{left_to_right, european_number};

            if (label.empty()) {
                return true;
            }

            // Rule 1
            auto classes = std::ranges::to<std::vector>(label | std::views::transform(bidi_class_of));
            auto is_rtl = classes.front() == right_to_left || classes.front() == arabic_letter;
            if (!is_rtl && classes.front() != left_to_right) {
                return false;
            }

            // Rules 3 and 6 skip trailing NSMs, and the first class isn't one.
            auto last =
                *std::ranges::find_if(classes | std::views::reverse, [](idna::bidi_class value) -> bool {
                    return value != nonspacing_mark;
                });
            if (is_rtl) {
                // Rules 2 to 4
                return std::ranges::all_of(classes,
                                           [&](idna::bidi_class value) -> bool {
                                               return std::ranges::contains(rtl_classes, value);
                                           }) &&
                       std::ranges::contains(rtl_ends, last) &&
                       !(std::ranges::contains(classes, european_number) &&
                         std::ranges::contains(classes, arabic_number));
            }

            // Rules 5 and 6
            return std::ranges::all_of(classes,
                                       [&](idna::bidi_class value) -> bool {
                                           return std::ranges::contains(ltr_classes, value);
                                       }) &&
                   std::ranges::contains(ltr_ends, last);
        }

        // UTS #46 section 4.1 for the flags to_ascii uses, except CheckBidi, which needs the whole domain
        auto validate(std::u32string_view label) -> std::expected<void, rei::error> {
            if (idna::normalize(label) != label) {
                return idna::invalid("label isn't in NFC");
            }
            if (label.starts_with(U"xn--")) {
                return idna::invalid("Punycode label decodes to another Punycode label");
            }
            if (!label.empty() && idna::find_range(idna::combining_marks, label.front()) != nullptr) {
                return idna::invalid("label starts with a combining mark");
            }
            if (!std::ranges::all_of(label, [](char32_t code_point) -> bool {
                    return idna::mapping_of(code_point).status == status::valid;
                })) {
                return idna::invalid("label contains a disallowed code point");
            }
            if (!idna::satisfies_context_j(label)) {
                return idna::invalid("label breaks the ContextJ rules for joiners");
            }

            return {};
        }
    } // namespace

    auto to_ascii(std::string_view domain) -> std::expected<std::string, rei::error> {
        auto text = idna::normalize(idna::map(idna::decode_utf8(domain)));

        std::vector<std::u32string> labels;
        for (auto part : text | std::views::split(U'.')) {
            std::u32string label(part.begin(), part.end());
            if (label.starts_with(U"xn--")) {
                if (!std::ranges::all_of(label, is_ascii)) {
                    return idna::invalid("Punycode label contains a non-ASCII code point");
                }

                auto decoded = idna::punycode_decode(std::u32string_view{label}.substr(4));
                if (!decoded) {
                    return idna::invalid("label is not valid Punycode");
                }
                if (std::ranges::all_of(*decoded, is_ascii)) {
                    return idna::invalid("Punycode label decodes to an empty or ASCII label");
                }

                label = std::move(*decoded);
            }

            if (auto valid = idna::validate(label); !valid) {
                return std::unexpected(valid.error());
            }

            labels.push_back(std::move(label));
        }

        // CheckBidi applies to every label once any of them has a right-to-left code point.
        auto is_bidi_domain = std::ranges::any_of(labels | std::views::join, [](char32_t code_point) -> bool {
            auto value = idna::bidi_class_of(code_point);
            return value == bidi_class::right_to_left || value == bidi_class::arabic_letter ||
                   value == bidi_class::arabic_number;
        });
        if (is_bidi_domain && !std::ranges::all_of(labels, satisfies_bidi_rule)) {
            return idna::invalid("label breaks the Bidi rules");
        }

        std::string ascii;
        for (auto i = 0uz; i < labels.size(); i++) {
            if (i > 0) {
                ascii += '.';
            }

            if (std::ranges::all_of(labels[i], is_ascii)) {
                std::ranges::transform(labels[i], std::back_inserter(ascii), [](char32_t code_point) -> char {
                    return static_cast<char>(code_point);
                });
                continue;
            }

            auto encoded = idna::punycode_encode(labels[i]);
            if (!encoded) {
                return idna::invalid("label is too long to encode as Punycode");
            }

            ascii += "xn--" + *encoded;
        }

        return ascii;
    }
} // namespace rei::core::detail::idna
