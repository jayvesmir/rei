#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "rei/core/error.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/json.hpp"
#include "rei/formats/json/decimal.hpp"
#include "rei/formats/json/detail.hpp"
#include "rei/formats/json/simd.hpp"

namespace rei::formats::json {
    namespace {
        namespace simd = detail::simd;

        constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";
        constexpr auto padding = 128uz;
        constexpr auto slack = 32uz;
        constexpr auto longest_integer = 19;
        constexpr auto longest_exponent = 5;
        constexpr auto short_block = 4uz;
        constexpr auto scalar_digits = 3;
        constexpr auto remembered_keys = 512uz;
        constexpr auto worth_guessing = 1024uz;
        constexpr auto longest_guess = (simd::width * 2) - 1;
        constexpr auto not_hex = 0x10000U;

        constexpr auto escape_table() noexcept -> std::array<char, 256> {
            std::array<char, 256> table{};
            table['"'] = '"';
            table['\\'] = '\\';
            table['/'] = '/';
            table['b'] = '\b';
            table['f'] = '\f';
            table['n'] = '\n';
            table['r'] = '\r';
            table['t'] = '\t';
            return table;
        }

        constexpr auto hex_table() noexcept -> std::array<std::uint32_t, 256> {
            std::array<std::uint32_t, 256> table{};
            table.fill(not_hex);

            for (auto digit = 0U; digit < 10; digit++) {
                table['0' + digit] = digit;
            }
            for (auto digit = 0U; digit < 6; digit++) {
                table['a' + digit] = 10 + digit;
                table['A' + digit] = 10 + digit;
            }

            return table;
        }

        constexpr auto unescaped = escape_table();
        constexpr auto hex_values = hex_table();

        constexpr auto byte(char unit) noexcept -> std::uint8_t {
            return static_cast<std::uint8_t>(unit);
        }

        constexpr auto is_digit(char unit) noexcept -> bool {
            return static_cast<std::uint8_t>(unit - '0') <= 9;
        }

        constexpr auto digit_of(char unit) noexcept -> std::uint64_t {
            return static_cast<std::uint8_t>(unit - '0');
        }

        constexpr auto is_exponent(char unit) noexcept -> bool {
            return (unit | 0x20) == 'e';
        }

        constexpr auto hex_quad(const char* from) noexcept -> std::uint32_t {
            return (hex_values[byte(from[0])] << 12) | (hex_values[byte(from[1])] << 8) |
                   (hex_values[byte(from[2])] << 4) | hex_values[byte(from[3])];
        }

        constexpr auto encode(char* into, std::uint32_t scalar) noexcept -> std::size_t {
            if (scalar < 0x80) {
                into[0] = static_cast<char>(scalar);
                return 1;
            }
            if (scalar < 0x800) {
                into[0] = static_cast<char>(0xC0 | (scalar >> 6));
                into[1] = static_cast<char>(0x80 | (scalar & 0x3F));
                return 2;
            }
            if (scalar < 0x10000) {
                into[0] = static_cast<char>(0xE0 | (scalar >> 12));
                into[1] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
                into[2] = static_cast<char>(0x80 | (scalar & 0x3F));
                return 3;
            }

            into[0] = static_cast<char>(0xF0 | (scalar >> 18));
            into[1] = static_cast<char>(0x80 | ((scalar >> 12) & 0x3F));
            into[2] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
            into[3] = static_cast<char>(0x80 | (scalar & 0x3F));
            return 4;
        }

        auto skip_digits(const char* from, const char* last) noexcept -> const char* {
            while (from != last && is_digit(*from)) {
                from++;
            }

            return from;
        }

        auto shown(std::string_view token) -> std::string {
            constexpr auto longest = 32uz;
            return token.size() <= longest ? std::string{token}
                                           : std::format("{}...", token.substr(0, longest));
        }

        class parser {
          private:
            std::string_view m_text;
            std::size_t m_max_depth;

            const char* m_segment{};
            const char* m_limit{};
            const char* m_stop{};
            std::size_t m_segment_offset{};
            std::array<char, padding * 2> m_tail;

            detail::arena* m_arena{};
            char* m_bytes{};
            char* m_low{};
            detail::node* m_floor{};
            std::size_t m_chunk{};
            std::size_t m_slots{};
            std::size_t m_written{};

            std::array<detail::node, 192> m_inline;
            detail::node* m_spilled{};
            detail::node* m_base{};
            detail::node* m_top{};
            detail::node* m_ceiling{};
            std::size_t m_frame{};
            std::size_t m_depth{};
            std::size_t m_step{};
            std::size_t m_history{};
            bool m_guessing{};
            std::array<std::uint8_t, remembered_keys> m_lengths;

            std::size_t m_failed_at{};
            std::string m_reason;
            formats::error_code m_code{formats::error_code::invalid_syntax};

            [[nodiscard]] auto _offset(const char* cursor) const noexcept -> std::size_t {
                return static_cast<std::size_t>(cursor - m_segment) + m_segment_offset;
            }

            REI_JSON_OUTLINE auto _fail(const char* cursor, std::string_view reason,
                                        formats::error_code code = formats::error_code::invalid_syntax)
                -> const char* {
                m_failed_at = _offset(cursor);
                m_reason = reason;
                m_code = code;
                return nullptr;
            }

            REI_JSON_OUTLINE auto _fail_depth(const char* cursor) -> const char* {
                return _fail(cursor, std::format("the document nests deeper than {} levels", m_max_depth),
                             formats::error_code::depth_exceeded);
            }

            REI_JSON_OUTLINE auto _fail_range(const char* cursor, const char* end) -> const char* {
                return _fail(cursor, std::format("{} is out of range for a number",
                                                 shown(std::string_view{cursor, end})));
            }

            REI_JSON_OUTLINE auto _fail_surrogate(const char* from, std::string_view missing) -> const char* {
                return _fail(from, std::format("the surrogate '{}' has no {} half", std::string_view{from, 6},
                                               missing));
            }

            REI_JSON_OUTLINE auto _fail_escape(const char* from) -> const char* {
                const auto unit = byte(from[1]);
                return unit > ' ' && unit < 0x7F
                           ? _fail(from, std::format("'\\{}' is not an escape", from[1]))
                           : _fail(from, R"('\' must be followed by an escape character)");
            }

            REI_JSON_OUTLINE auto _fail_text(const char* from) -> const char* {
                return from == m_stop
                           ? _fail(from, "the string is not closed")
                           : _fail(from, std::format("control character U+{:04X} in a string", byte(*from)));
            }

            [[nodiscard]] auto _failure() const -> std::unexpected<rei::error> {
                const auto before = m_text.substr(0, m_failed_at);
                const auto newline = before.rfind('\n');
                const auto start = newline == std::string_view::npos ? 0 : newline + 1;

                const auto line = 1 + std::ranges::count(before, '\n');
                const auto column = 1 + std::ranges::count_if(before.substr(start), [](char unit) -> bool {
                                        return !detail::is_continuation(unit);
                                    });

                return std::unexpected(rei::error{m_code, std::format("{}:{}: {}", line, column, m_reason)});
            }

            [[nodiscard]] REI_JSON_OUTLINE auto _into_tail(const char* cursor) noexcept -> const char* {
                const auto passed = cursor - m_limit;

                m_segment = m_tail.data();
                m_segment_offset = m_text.size() - padding;
                m_stop = m_tail.data() + padding;
                m_limit = m_tail.data() + m_tail.size();

                return m_tail.data() + passed;
            }

            [[nodiscard]] REI_JSON_OUTLINE auto _skip_run(const char* cursor, std::size_t level) noexcept
                -> const char* {
                const auto* line = cursor;

                while (true) {
                    const auto spaces = simd::leading_spaces(cursor);
                    if (spaces == simd::width) {
                        cursor += simd::width;
                    } else {
                        cursor += spaces;
                        if (*cursor != '\n' && *cursor != '\r' && *cursor != '\t') {
                            m_step = level == 0 ? m_step : static_cast<std::size_t>(cursor - line) / level;
                            return cursor;
                        }

                        cursor++;
                        line = cursor;
                    }

                    if (cursor >= m_limit) [[unlikely]] {
                        cursor = _into_tail(cursor);
                        line = cursor;
                    }
                }
            }

            [[nodiscard]] REI_JSON_INLINE auto _skip(const char* cursor, std::size_t level) noexcept -> const
                char* {
                if (cursor >= m_limit) [[unlikely]] {
                    cursor = _into_tail(cursor);
                }

                if (byte(*cursor) > ' ') {
                    return cursor;
                }
                if (*cursor == ' ' && byte(cursor[1]) > ' ') {
                    return cursor + 1;
                }

                const auto indent = std::min(level * m_step, simd::longest_indent);
                const auto* indented = cursor + 1 + indent;
                if (*cursor == '\n' && simd::is_indent(cursor + 1, indent) && byte(*indented) > ' ')
                    [[likely]] {
                    return indented;
                }

                return _skip_run(cursor, level);
            }

            REI_JSON_OUTLINE auto _spill() -> void {
                const auto used = static_cast<std::size_t>(m_top - m_base);
                const auto capacity = used * 2;
                auto* grown = static_cast<detail::node*>(detail::allocate(capacity * sizeof(detail::node)));
                std::memcpy(static_cast<void*>(grown), m_base, used * sizeof(detail::node));
                detail::deallocate(m_spilled);

                m_spilled = grown;
                m_base = grown;
                m_top = m_base + used;
                m_ceiling = m_base + capacity;
            }

            [[nodiscard]] REI_JSON_INLINE auto _slot() -> detail::node& {
                if (m_top == m_ceiling) [[unlikely]] {
                    _spill();
                }

                auto& made = *m_top;
                m_top++;
                return made;
            }

            auto _settle(const detail::region& space) noexcept -> void {
                m_written += static_cast<std::size_t>(m_bytes - m_low);
                m_low = space.low;
                m_bytes = space.low;
                m_floor = reinterpret_cast<detail::node*>(space.high);
                m_chunk = static_cast<std::size_t>(space.high - space.low);
            }

            auto _begin(const char* cursor) -> void {
                const auto pending = static_cast<std::size_t>(m_stop - cursor);
                const auto space = detail::open_arena(pending + (pending / 2) + (slack * 8));
                m_arena = space.owner;
                _settle(space);
            }

            REI_JSON_OUTLINE auto _extend(std::size_t needed) -> void {
                _settle(detail::extend_arena(*m_arena, std::max(needed, m_chunk * 2)));
            }

            [[nodiscard]] REI_JSON_INLINE auto _block(const char* cursor, std::size_t count)
                -> detail::node* {
                const auto bytes = count * sizeof(detail::node);
                const auto pending = static_cast<std::size_t>(m_stop - cursor);
                const auto gap = static_cast<std::size_t>(reinterpret_cast<char*>(m_floor) - m_bytes);

                if (gap < bytes + pending + slack) [[unlikely]] {
                    _extend(bytes + pending + slack);
                }

                m_floor -= count;
                return m_floor;
            }

            [[nodiscard]] auto _unicode(const char* from, char*& into) -> const char* {
                const auto unit = hex_quad(from + 2);
                if (unit >= not_hex) {
                    return _fail(from, R"('\u' must be followed by four hexadecimal digits)");
                }
                if (unit >= 0xDC00 && unit <= 0xDFFF) {
                    return _fail_surrogate(from, "first");
                }
                if (unit < 0xD800 || unit > 0xDBFF) {
                    into += encode(into, unit);
                    return from + 6;
                }

                const auto second = from[6] == '\\' && from[7] == 'u' ? hex_quad(from + 8) : not_hex;
                if (second < 0xDC00 || second > 0xDFFF) {
                    return _fail_surrogate(from, "second");
                }

                into += encode(into, 0x10000 + ((unit - 0xD800) << 10) + (second - 0xDC00));
                return from + 12;
            }

            [[nodiscard]] auto _escape(const char* from, char*& into) -> const char* {
                if (from[1] == 'u') {
                    return _unicode(from, into);
                }

                const auto plain = unescaped[byte(from[1])];
                if (plain == 0) {
                    return _fail_escape(from);
                }

                *into = plain;
                into++;
                return from + 2;
            }

            [[nodiscard]] auto _multibyte(const char* from, char*& into) -> const char* {
                do {
                    if (from >= m_limit) [[unlikely]] {
                        from = _into_tail(from);
                    }

                    const auto length = detail::sequence_length(from);
                    if (length == 0) {
                        return _fail(from, "the string is not valid UTF-8");
                    }

                    std::memcpy(into, from, 4);
                    from += length;
                    into += length;
                } while (byte(*from) >= 0x80);

                return from;
            }

            [[nodiscard]] REI_JSON_OUTLINE auto _long_string(const char* from, char* into, detail::node& made)
                -> const char* {
                while (*from != '"') {
                    if (from >= m_limit) [[unlikely]] {
                        from = _into_tail(from);
                    }

                    const auto unit = byte(*from);
                    if (unit == '\\') {
                        from = _escape(from, into);
                    } else if (unit >= 0x80) {
                        from = _multibyte(from, into);
                    } else if (unit < ' ') {
                        return _fail_text(from);
                    } else {
                        const auto run = simd::copy_plain(from, into);
                        from += run;
                        into += run;
                    }

                    if (from == nullptr) {
                        return nullptr;
                    }
                }

                made = {
                    .meta = detail::tag(kind::string, 0, static_cast<std::size_t>(into - m_bytes)),
                    .text = m_bytes,
                };
                m_bytes = into;
                return from + 1;
            }

            [[nodiscard]] REI_JSON_INLINE auto _rest_of_string(const char* from, char* into,
                                                               simd::probe found, detail::node& made) -> const
                char* {
                auto length = 0uz;
                if (!found.closes() && found.plain() == simd::width) {
                    length = simd::width;
                    found = simd::probe_text(from + length, into + length);
                }
                if (!found.closes()) [[unlikely]] {
                    length += found.plain();
                    return _long_string(from + length, into + length, made);
                }

                length += found.reach();
                made = {.meta = detail::tag(kind::string, 0, length), .text = into};
                m_bytes = into + length;
                return from + length + 1;
            }

            [[nodiscard]] REI_JSON_INLINE auto _string(const char* cursor, detail::node& made) -> const
                char* {
                return _rest_of_string(cursor + 1, m_bytes, simd::probe_text(cursor + 1, m_bytes), made);
            }

            REI_JSON_INLINE auto _remember(std::size_t length) noexcept -> void {
                m_history = ((m_history << 4) ^ length) % remembered_keys;
            }

            [[nodiscard]] REI_JSON_INLINE auto _name(const char* cursor, detail::node& made) -> const char* {
                if (!m_guessing) {
                    return _string(cursor, made);
                }

                const auto* from = cursor + 1;
                auto* into = m_bytes;
                const auto guess = std::size_t{m_lengths[m_history]};
                const auto first = simd::probe_text(from, into);

                auto found = first;
                auto within = guess;
                auto clear = true;
                if (guess >= simd::width) {
                    clear = first.plain() == simd::width;
                    found = simd::probe_text(from + simd::width, into + simd::width);
                    within = guess - simd::width;
                }

                if (clear && found.plain_until(within) && from[guess] == '"') [[likely]] {
                    made = {.meta = detail::tag(kind::string, 0, guess), .text = into};
                    m_bytes = into + guess;
                    _remember(guess);
                    return from + guess + 1;
                }

                const auto* end = _rest_of_string(from, into, first, made);
                if (end != nullptr) {
                    const auto length = std::min(detail::size_of(made), longest_guess);
                    m_lengths[m_history] = static_cast<std::uint8_t>(length);
                    _remember(length);
                }

                return end;
            }

            [[nodiscard]] REI_JSON_OUTLINE auto _long_number(const char* cursor, detail::node& made) -> const
                char* {
                const auto* last = m_stop;
                const auto negative = *cursor == '-';
                const auto* integral = cursor + (negative ? 1 : 0);
                const auto* end = skip_digits(integral, last);
                const auto leading = static_cast<std::int64_t>(end - integral);

                auto whole = true;
                std::int64_t zeros = 0;
                if (end != last && *end == '.') {
                    const auto* fraction = end + 1;
                    end = skip_digits(fraction, last);
                    if (end == fraction) {
                        return _fail(end, "expected a digit after the decimal point");
                    }

                    whole = false;
                    zeros =
                        std::ranges::find_if(fraction, end, [](char unit) -> bool { return unit != '0'; }) -
                        fraction;
                }

                std::int64_t written = 0;
                if (end != last && is_exponent(*end)) {
                    const auto* power = end + 1;
                    const auto minus = power != last && *power == '-';
                    power += power != last && (*power == '-' || *power == '+') ? 1 : 0;
                    end = skip_digits(power, last);
                    if (end == power) {
                        return _fail(end, "expected a digit in the exponent");
                    }

                    whole = false;
                    constexpr std::int64_t furthest = 100'000'000;
                    for (const auto* digit = power; digit != end && written < furthest; digit++) {
                        written = (written * 10) + static_cast<std::int64_t>(digit_of(*digit));
                    }
                    written = minus ? -written : written;
                }

                std::uint64_t natural{};
                if (whole && !negative && std::from_chars(integral, end, natural).ec == std::errc{}) {
                    made = detail::integer_node(natural);
                    return end;
                }

                double number{};
                if (std::from_chars(cursor, end, number).ec == std::errc::result_out_of_range) {
                    const auto magnitude = written + (*integral == '0' ? -zeros : leading);
                    if (magnitude > 0) {
                        return _fail_range(cursor, end);
                    }

                    number = negative ? -0.0 : 0.0;
                }

                made = {.meta = detail::tag(kind::floating), .floating = number};
                return end;
            }

            REI_JSON_INLINE static auto _absorb(std::uint64_t& digits, const char*& end) noexcept
                -> std::int32_t {
                const auto word = detail::load_eight(end);
                const auto count = detail::digit_count(word);
                if (count != 0) {
                    digits = (digits * detail::small_powers_of_ten[static_cast<std::size_t>(count)]) +
                             detail::leading_digits(word, count);
                    end += count;
                }

                return count;
            }

            [[nodiscard]] REI_JSON_OUTLINE auto _number(const char* cursor, detail::node& made) -> const
                char* {
                const auto negative = *cursor == '-';
                const auto* integral = cursor + (negative ? 1 : 0);

                auto digits = digit_of(*integral);
                if (digits > 9) {
                    return _fail(integral, "expected a digit after '-'");
                }

                const auto* end = integral + 1;
                if (is_digit(*end)) {
                    if (digits == 0) {
                        return _fail(integral, "a number can't start with a zero");
                    }

                    while (is_digit(*end) && end - integral < scalar_digits) {
                        digits = (digits * 10) + digit_of(*end);
                        end++;
                    }

                    if (is_digit(*end) && _absorb(digits, end) == 8 && _absorb(digits, end) == 8 &&
                        is_digit(*end)) {
                        return _long_number(cursor, made);
                    }
                }

                if (*end != '.' && !is_exponent(*end)) {
                    if (!negative) {
                        made = detail::integer_node(digits);
                    } else if (digits <= (1ULL << 63)) {
                        made = {.meta = detail::tag(kind::integer), .natural = 0 - digits};
                    } else {
                        made = {
                            .meta = detail::tag(kind::floating),
                            .floating = -static_cast<double>(digits),
                        };
                    }

                    return end;
                }

                std::int64_t exponent = 0;
                if (*end == '.') {
                    const auto* fraction = end + 1;
                    auto budget =
                        longest_integer - (digits == 0 ? 0 : static_cast<std::int32_t>(end - integral));
                    end = fraction;

                    while (budget >= 8 && detail::is_eight_digits(detail::load_eight(end))) {
                        digits = (digits * 100'000'000) + detail::eight_digits(detail::load_eight(end));
                        end += 8;
                        budget -= 8;
                    }

                    if (_absorb(digits, end) > budget) {
                        return _long_number(cursor, made);
                    }
                    if (end == fraction) {
                        return _fail(fraction, "expected a digit after the decimal point");
                    }

                    exponent = fraction - end;
                }

                if (is_exponent(*end)) {
                    const auto* power = end + 1;
                    const auto minus = *power == '-';
                    power += minus || *power == '+' ? 1 : 0;

                    std::uint64_t written = 0;
                    end = power;
                    const auto count = _absorb(written, end);
                    if (count == 0) {
                        return _fail(power, "expected a digit in the exponent");
                    }
                    if (count > longest_exponent) {
                        return _long_number(cursor, made);
                    }

                    exponent +=
                        minus ? -static_cast<std::int64_t>(written) : static_cast<std::int64_t>(written);
                }

                const auto number = detail::scale(digits, exponent);
                if (std::isinf(number)) {
                    return _fail_range(cursor, end);
                }

                made = {.meta = detail::tag(kind::floating), .floating = negative ? -number : number};
                return end;
            }

            [[nodiscard]] REI_JSON_OUTLINE auto _literal(const char* cursor, detail::node& made) -> const
                char* {
                if (std::memcmp(cursor, "true", 4) == 0) {
                    made = {.meta = detail::tag(kind::boolean), .natural = 1};
                    return cursor + 4;
                }
                if (std::memcmp(cursor, "false", 5) == 0) {
                    made = {.meta = detail::tag(kind::boolean), .natural = 0};
                    return cursor + 5;
                }
                if (std::memcmp(cursor, "null", 4) == 0) {
                    made = {.meta = detail::tag(kind::null), .natural = 0};
                    return cursor + 4;
                }

                return _fail(cursor, "expected a value");
            }

            [[nodiscard]] REI_JSON_INLINE auto _scalar(const char* cursor) -> const char* {
                auto& made = _slot();
                const auto unit = *cursor;

                if (unit == '"') {
                    return _string(cursor, made);
                }
                if (unit == '-' || is_digit(unit)) {
                    return _number(cursor, made);
                }

                return _literal(cursor, made);
            }

            [[nodiscard]] REI_JSON_INLINE auto _open(const char* cursor, json::kind kind) -> bool {
                if (m_depth == m_max_depth) [[unlikely]] {
                    _fail_depth(cursor);
                    return false;
                }

                auto& frame = _slot();
                frame = {.meta = detail::tag(kind), .natural = m_frame};
                m_frame = static_cast<std::size_t>(&frame - m_base);
                m_depth++;
                return true;
            }

            REI_JSON_INLINE auto _close(const char* cursor) -> void {
                auto* frame = m_base + m_frame;
                auto* items = frame + 1;
                const auto count = static_cast<std::size_t>(m_top - items);
                const auto kind = detail::kind_of(*frame);
                const auto outer = static_cast<std::size_t>(frame->natural);

                *frame = {.meta = detail::tag(kind), .natural = 0};
                if (count != 0) {
                    const auto rooted = m_depth == 1;
                    auto* block = _block(cursor, count + (rooted ? 1 : 0));
                    if (rooted) {
                        ::new (static_cast<void*>(block)) detail::header{.owned = m_arena, .capacity = count};
                        block++;
                    }

                    if (count == 1) {
                        block[0] = items[0];
                    } else if (count <= short_block) {
                        const auto first = items[0];
                        const auto second = items[1];
                        const auto third = items[count - 2];
                        const auto fourth = items[count - 1];
                        block[0] = first;
                        block[1] = second;
                        block[count - 2] = third;
                        block[count - 1] = fourth;
                    } else {
                        std::memcpy(static_cast<void*>(block), items, count * sizeof(detail::node));
                    }

                    *frame = {
                        .meta = detail::tag(kind, rooted ? detail::owner_bit : 0,
                                            kind == kind::object ? count / 2 : count),
                        .slots = reinterpret_cast<json::value*>(block),
                    };
                    m_slots += count;
                }

                m_top = items;
                m_frame = outer;
                m_depth--;
            }

            [[nodiscard]] auto _document(const char* cursor) -> const char* {
                if (*cursor == '[') {
                    goto open_array;
                }
                if (*cursor == '{') {
                    goto open_object;
                }

                return _scalar(cursor);

            open_array:
                if (!_open(cursor, kind::array)) {
                    return nullptr;
                }

                cursor = _skip(cursor + 1, m_depth);
                if (*cursor == ']') {
                    cursor++;
                    goto close;
                }

            array_item:
                if (*cursor == '[') {
                    goto open_array;
                }
                if (*cursor == '{') {
                    goto open_object;
                }

                cursor = _scalar(cursor);
                if (cursor == nullptr) {
                    return nullptr;
                }

            array_next:
                cursor = _skip(cursor, m_depth - 1);
                if (*cursor == ',') {
                    cursor = _skip(cursor + 1, m_depth);
                    goto array_item;
                }
                if (*cursor == ']') {
                    cursor++;
                    goto close;
                }

                return _fail(cursor, "expected ',' or ']' after an array element");

            open_object:
                if (!_open(cursor, kind::object)) {
                    return nullptr;
                }

                cursor = _skip(cursor + 1, m_depth);
                if (*cursor == '}') {
                    cursor++;
                    goto close;
                }

            object_name:
                if (*cursor != '"') {
                    return _fail(cursor, "expected a member name");
                }

                cursor = _name(cursor, _slot());
                if (cursor == nullptr) {
                    return nullptr;
                }

                cursor = _skip(cursor, m_depth);
                if (*cursor != ':') {
                    return _fail(cursor, "expected ':' after a member name");
                }

                cursor = _skip(cursor + 1, m_depth);
                if (*cursor == '[') {
                    goto open_array;
                }
                if (*cursor == '{') {
                    goto open_object;
                }

                cursor = _scalar(cursor);
                if (cursor == nullptr) {
                    return nullptr;
                }

            object_next:
                cursor = _skip(cursor, m_depth - 1);
                if (*cursor == ',') {
                    cursor = _skip(cursor + 1, m_depth);
                    goto object_name;
                }
                if (*cursor == '}') {
                    cursor++;
                    goto close;
                }

                return _fail(cursor, "expected ',' or '}' after an object member");

            close:
                _close(cursor);
                if (m_depth == 0) {
                    return cursor;
                }
                if (detail::kind_of(m_base[m_frame]) == kind::object) {
                    goto object_next;
                }

                goto array_next;
            }

            [[nodiscard]] auto _finish() -> json::value {
                auto root = *m_base;
                if (detail::is_owner(root)) {
                    m_arena->slots = m_slots;
                    m_arena->text = m_written + static_cast<std::size_t>(m_bytes - m_low);
                    return detail::access::wrap(root);
                }

                if (detail::kind_of(root) == kind::string) {
                    root = detail::text_node(detail::text_of(root));
                }
                if (m_arena != nullptr) {
                    detail::close_arena(*m_arena);
                }

                return detail::access::wrap(root);
            }

          public:
            parser(std::string_view text, const json::parse_options& options)
                : m_text(text), m_max_depth(options.max_depth), m_guessing(text.size() >= worth_guessing) {
                m_base = m_inline.data();
                m_top = m_base;
                m_ceiling = m_base + m_inline.size();
                if (m_guessing) {
                    m_lengths.fill(0);
                }

                const auto length = std::min(text.size(), padding);
                m_tail.fill('\0');
                std::ranges::copy(text.substr(text.size() - length), m_tail.data());

                if (text.size() <= padding) {
                    m_segment = m_tail.data();
                    m_stop = m_tail.data() + length;
                    m_limit = m_tail.data() + m_tail.size();
                } else {
                    m_segment = text.data();
                    m_stop = text.data() + text.size();
                    m_limit = m_stop - padding;
                }
            }

            parser(const parser&) = delete;
            auto operator=(const parser&) -> parser& = delete;

            ~parser() {
                if (m_spilled != nullptr) {
                    detail::deallocate(m_spilled);
                }
            }

            [[nodiscard]] auto run() -> std::expected<json::value, rei::error> {
                const auto* cursor = _skip(m_segment + (m_text.starts_with(byte_order_mark) ? 3 : 0), 0);
                if (*cursor == '[' || *cursor == '{' || *cursor == '"') {
                    _begin(cursor);
                }

                cursor = _document(cursor);
                if (cursor != nullptr) {
                    cursor = _skip(cursor, 0);
                    if (cursor != m_stop || m_segment != m_tail.data()) {
                        cursor = _fail(cursor, "unexpected text after the document");
                    }
                }

                if (cursor == nullptr) {
                    if (m_arena != nullptr) {
                        detail::close_arena(*m_arena);
                    }

                    return _failure();
                }

                return _finish();
            }
        };
    } // namespace

    auto parse(std::string_view text, const json::parse_options& options)
        -> std::expected<json::value, rei::error> {
        return parser{text, options}.run();
    }
} // namespace rei::formats::json
