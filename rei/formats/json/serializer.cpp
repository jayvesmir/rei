#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zmij.h"

#include "rei/core/error.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/json.hpp"
#include "rei/formats/json/decimal.hpp"
#include "rei/formats/json/detail.hpp"
#include "rei/formats/json/simd.hpp"

namespace rei::formats::json {
    namespace {
        namespace simd = detail::simd;

        constexpr auto headroom = 96uz;
        constexpr auto smallest_estimate = 256uz;
        constexpr auto bytes_per_slot = 14uz;
        constexpr auto fixed_limit = 1e16;
        constexpr auto bulk_text = 64uz;
        constexpr auto near_frames = 32uz;
        constexpr std::string_view hex_digits = "0123456789ABCDEF";

        constexpr auto escape_table() noexcept -> std::array<char, 256> {
            std::array<char, 256> table{};
            table['"'] = '"';
            table['\\'] = '\\';
            table['\b'] = 'b';
            table['\f'] = 'f';
            table['\n'] = 'n';
            table['\r'] = 'r';
            table['\t'] = 't';
            return table;
        }

        constexpr auto short_escapes = escape_table();

        struct frame {
            const detail::node* first;
            const detail::node* next;
            const detail::node* last;
            char closer;
        };

        auto bound(const detail::node& held) noexcept -> std::size_t {
            if (detail::kind_of(held) != kind::string) {
                return 0;
            }

            const auto size = detail::text_of(held).size();
            return (held.meta & detail::plain_bit) != 0 ? size : size * 6;
        }

        auto escaped(char* into, std::string_view text) noexcept -> char* {
            const auto* from = text.data();
            const auto* last = from + text.size();

            while (true) {
                const auto run =
                    std::min(simd::copy_plain(from, into), static_cast<std::size_t>(last - from));
                from += run;
                into += run;
                if (from == last) {
                    return into;
                }
                if (run == simd::width) {
                    continue;
                }

                const auto unit = static_cast<std::uint8_t>(*from);
                if (unit >= 0x80) {
                    const auto length = detail::sequence_length(from);
                    if (length == 0 || std::cmp_greater(length, last - from)) {
                        return nullptr;
                    }

                    std::memcpy(into, from, 4);
                    from += length;
                    into += length;
                    continue;
                }

                into[0] = '\\';
                if (short_escapes[unit] != 0) {
                    into[1] = short_escapes[unit];
                    into += 2;
                } else {
                    into[1] = 'u';
                    into[2] = '0';
                    into[3] = '0';
                    into[4] = hex_digits[unit >> 4];
                    into[5] = hex_digits[unit & 0xF];
                    into += 6;
                }

                from++;
            }
        }

        REI_JSON_OUTLINE auto checked(char* into, const detail::node& held) noexcept -> char* {
            std::array<char, sizeof(detail::node) + simd::width> local{};
            auto text = detail::text_of(held);
            if ((held.meta & detail::inline_bit) != 0) {
                std::memcpy(local.data(), &held, sizeof(held));
                text = {local.data() + detail::inline_offset, text.size()};
            }

            *into = '"';
            into = escaped(into + 1, text);
            if (into == nullptr) {
                return nullptr;
            }

            *into = '"';
            return into + 1;
        }

        REI_JSON_INLINE auto quoted(char* into, const detail::node& held) noexcept -> char* {
            if ((held.meta & (detail::plain_bit | detail::inline_bit)) != detail::plain_bit) {
                return checked(into, held);
            }

            const auto size = detail::size_of(held);
            into[0] = '"';
            into++;

            if (size <= bulk_text) [[likely]] {
                for (auto copied = 0uz; copied < size; copied += simd::width) {
                    simd::move_block(held.text + copied, into + copied);
                }
            } else {
                std::memcpy(into, held.text, size);
            }

            into[size] = '"';
            return into + size + 1;
        }

        auto floating(char* into, double number) noexcept -> char* {
            auto* end = zmij::write(into, zmij::double_buffer_size, number);

            const auto magnitude = std::fabs(number);
            if (magnitude < fixed_limit &&
                static_cast<double>(static_cast<std::int64_t>(magnitude)) == magnitude) {
                end[0] = '.';
                end[1] = '0';
                end += 2;
            }

            return end;
        }

        auto unwritable(const detail::node& held) noexcept -> std::string_view {
            if (detail::kind_of(held) != kind::floating) {
                return "the string is not valid UTF-8";
            }

            return std::isnan(held.floating) ? "nan can't be written as JSON"
                                             : "an infinite number can't be written as JSON";
        }

        class writer {
          private:
            std::array<frame, near_frames> m_near;
            std::vector<frame> m_far;
            std::span<frame> m_frames{m_near};
            std::size_t m_depth{};
            std::size_t m_written{};
            std::size_t m_needed{};
            std::optional<rei::error> m_error;

            REI_JSON_OUTLINE auto _deepen() -> void {
                if (m_far.empty()) {
                    m_far.assign(m_near.begin(), m_near.end());
                }

                m_far.resize(m_far.size() * 2);
                m_frames = m_far;
            }

            REI_JSON_INLINE auto _push(const frame& level) -> void {
                if (m_depth == m_frames.size()) [[unlikely]] {
                    _deepen();
                }

                m_frames[m_depth] = level;
                m_depth++;
            }

            REI_JSON_OUTLINE auto _fail(std::string_view reason, bool at_key) -> void {
                std::string where;

                for (auto i = 1uz; i < m_depth; i++) {
                    const auto& level = m_frames[i];
                    const auto position = static_cast<std::size_t>(level.next - level.first) - 1;
                    if (level.closer == ']') {
                        where += std::format("[{}]", position);
                    } else if (i + 1 < m_depth || !at_key) {
                        where += where.empty() ? "" : ".";
                        where += detail::text_of(level.first[position & ~1uz]);
                    }
                }

                m_error.emplace(formats::error_code::invalid_value,
                                where.empty() ? std::string{reason} : std::format("{}: {}", where, reason));
            }

            [[nodiscard]] REI_JSON_INLINE static auto _scalar(char* into, const detail::node& held) noexcept
                -> char* {
                switch (detail::kind_of(held)) {
                case kind::string:
                    return quoted(into, held);
                case kind::integer:
                    if ((held.meta & detail::unsigned_bit) == 0 && held.integer < 0) {
                        into[0] = '-';
                        return detail::write_natural(into + 1, 0 - held.natural);
                    }

                    return detail::write_natural(into, held.natural);
                case kind::floating:
                    return std::isfinite(held.floating) ? floating(into, held.floating) : nullptr;
                case kind::boolean:
                    std::memcpy(into, held.natural != 0 ? "true " : "false", 5);
                    return into + (held.natural != 0 ? 4 : 5);
                default:
                    std::memcpy(into, "null", 4);
                    return into + 4;
                }
            }

            auto _fill(char* data, std::size_t size) -> std::size_t {
                auto* into = data + m_written;
                const auto* limit = data + size;
                auto level = m_frames[m_depth - 1];

                while (true) {
                    if (level.next == level.last) {
                        m_depth--;
                        if (m_depth == 0) {
                            into--;
                            break;
                        }

                        into[-1] = level.closer;
                        into[0] = ',';
                        into++;
                        level = m_frames[m_depth - 1];
                        continue;
                    }

                    const auto named = level.closer == '}';
                    const auto& held = level.next[named ? 1 : 0];
                    m_needed = headroom + bound(held) + (named ? bound(*level.next) : 0);
                    if (std::cmp_less(limit - into, m_needed)) [[unlikely]] {
                        m_frames[m_depth - 1] = level;
                        break;
                    }

                    if (named) {
                        into = quoted(into, *level.next);
                        if (into == nullptr) [[unlikely]] {
                            level.next++;
                            m_frames[m_depth - 1] = level;
                            _fail("a key is not valid UTF-8", true);
                            break;
                        }

                        into[0] = ':';
                        into++;
                    }

                    level.next += named ? 2 : 1;

                    if (detail::kind_of(held) < kind::array) {
                        into = _scalar(into, held);
                        if (into == nullptr) [[unlikely]] {
                            m_frames[m_depth - 1] = level;
                            _fail(unwritable(held), false);
                            break;
                        }

                        into[0] = ',';
                        into++;
                        continue;
                    }

                    const auto keyed = detail::kind_of(held) == kind::object;
                    if (detail::size_of(held) == 0) {
                        std::memcpy(into, keyed ? "{}," : "[],", 3);
                        into += 3;
                        continue;
                    }

                    into[0] = keyed ? '{' : '[';
                    into++;

                    const auto* slots = detail::slots_of(held);
                    m_frames[m_depth - 1] = level;
                    level = {
                        .first = slots,
                        .next = slots,
                        .last = slots + detail::slot_count(held),
                        .closer = keyed ? '}' : ']',
                    };
                    _push(level);
                }

                m_written = m_error ? m_written : static_cast<std::size_t>(into - data);
                return m_written;
            }

          public:
            [[nodiscard]] auto run(const detail::node& root) -> std::expected<std::string, rei::error> {
                auto capacity = smallest_estimate;
                if (detail::kind_of(root) >= kind::array && detail::is_owner(root)) {
                    if (const auto* owned = detail::header_of(root).owned;
                        owned != nullptr && !owned->dirty) {
                        capacity += owned->text + (owned->slots * bytes_per_slot);
                    }
                }

                std::string out;
                _push({.first = &root, .next = &root, .last = &root + 1, .closer = '\0'});

                while (m_depth != 0 && !m_error) {
                    out.resize_and_overwrite(capacity, [&](char* data, std::size_t size) -> std::size_t {
                        return _fill(data, size);
                    });
                    capacity = std::max(capacity * 2, m_written + m_needed);
                }

                if (m_error) {
                    return std::unexpected(std::move(*m_error));
                }

                return out;
            }
        };
    } // namespace

    auto serialize(const json::value& value) -> std::expected<std::string, rei::error> {
        writer out;
        return out.run(detail::access::peek(value));
    }
} // namespace rei::formats::json
