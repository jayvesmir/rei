#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/core/utf8.hpp"
#include "rei/formats/dictionary.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/toml.hpp"

namespace rei::formats::toml {
    namespace {
        constexpr auto max_depth = 128uz;
        constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";

        using key_path = std::vector<std::string>;

        template <typename T>
        auto failure(std::expected<T, rei::error>& result) -> std::unexpected<rei::error> {
            return std::unexpected(std::move(result).error());
        }

        constexpr auto is_digit(char unit) noexcept -> bool {
            return unit >= '0' && unit <= '9';
        }

        constexpr auto is_hex_digit(char unit) noexcept -> bool {
            return is_digit(unit) || (unit >= 'a' && unit <= 'f') || (unit >= 'A' && unit <= 'F');
        }

        constexpr auto is_octal_digit(char unit) noexcept -> bool {
            return unit >= '0' && unit <= '7';
        }

        constexpr auto is_binary_digit(char unit) noexcept -> bool {
            return unit == '0' || unit == '1';
        }

        constexpr auto is_bare(char unit) noexcept -> bool {
            return is_digit(unit) || (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') ||
                   unit == '_' || unit == '-';
        }

        constexpr auto is_scalar(char unit) noexcept -> bool {
            return is_bare(unit) || unit == '+' || unit == '.' || unit == ':';
        }

        constexpr auto is_control(char unit) noexcept -> bool {
            const auto byte = static_cast<unsigned char>(unit);
            return (byte < 0x20 && unit != '\t') || byte == 0x7F;
        }

        template <typename Digit> auto is_run(std::string_view text, Digit digit) noexcept -> bool {
            if (text.empty() || !digit(text.front()) || !digit(text.back())) {
                return false;
            }

            for (auto i = 1uz; i + 1 < text.size(); i++) {
                const auto valid = text[i] == '_' ? text[i - 1] != '_' : digit(text[i]);
                if (!valid) {
                    return false;
                }
            }

            return true;
        }

        auto without_underscores(std::string_view text) -> std::string {
            std::string plain;
            std::ranges::copy_if(text, std::back_inserter(plain),
                                 [](char unit) -> bool { return unit != '_'; });
            return plain;
        }

        auto joined(std::span<const std::string> path) -> std::string {
            std::string name;

            for (const auto& part : path) {
                name += name.empty() ? "" : ".";
                name += part;
            }

            return name;
        }

        auto read_field(std::string_view digits, std::uint32_t highest) noexcept
            -> std::optional<std::uint32_t> {
            std::uint32_t field{};
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), field);

            if (!std::ranges::all_of(digits, is_digit) || error != std::errc{} || field > highest) {
                return std::nullopt;
            }

            return field;
        }

        auto read_date(std::string_view text) noexcept -> std::optional<toml::date> {
            if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
                return std::nullopt;
            }

            const auto year = read_field(text.substr(0, 4), 9999);
            const auto month = read_field(text.substr(5, 2), 12);
            const auto day = read_field(text.substr(8, 2), 31);
            if (!year || !month || !day) {
                return std::nullopt;
            }

            const toml::date date{std::chrono::year{static_cast<int>(*year)}, std::chrono::month{*month},
                                  std::chrono::day{*day}};
            return date.ok() ? std::optional{date} : std::nullopt;
        }

        auto read_time(std::string_view text) noexcept -> std::optional<toml::time> {
            if (text.size() < 5 || text[2] != ':') {
                return std::nullopt;
            }

            const auto hour = read_field(text.substr(0, 2), 23);
            const auto minute = read_field(text.substr(3, 2), 59);
            if (!hour || !minute) {
                return std::nullopt;
            }

            toml::time time{
                .hour = static_cast<std::uint8_t>(*hour),
                .minute = static_cast<std::uint8_t>(*minute),
            };
            if (text.size() == 5) {
                return time;
            }

            if (text.size() < 8 || text[5] != ':') {
                return std::nullopt;
            }

            const auto second = read_field(text.substr(6, 2), 59);
            if (!second) {
                return std::nullopt;
            }

            time.second = static_cast<std::uint8_t>(*second);
            if (text.size() == 8) {
                return time;
            }

            const auto fraction = text.substr(9);
            if (text[8] != '.' || fraction.empty() || !std::ranges::all_of(fraction, is_digit)) {
                return std::nullopt;
            }

            std::string nanoseconds{fraction.substr(0, 9)};
            nanoseconds.resize(9, '0');
            time.nanosecond = *read_field(nanoseconds, 999'999'999);
            return time;
        }

        auto read_offset(std::string_view text) noexcept -> std::optional<std::chrono::minutes> {
            if (text == "Z" || text == "z") {
                return std::chrono::minutes{0};
            }

            if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':') {
                return std::nullopt;
            }

            const auto hours = read_field(text.substr(1, 2), 23);
            const auto minutes = read_field(text.substr(4, 2), 59);
            if (!hours || !minutes) {
                return std::nullopt;
            }

            const std::chrono::minutes offset{(*hours * 60) + *minutes};
            return text[0] == '-' ? -offset : offset;
        }

        enum class mark : std::uint8_t {
            closed = 1,
            defined = 2,
        };

        struct marks {
            std::uint8_t own{};
            std::uint8_t inherited{};
            formats::dictionary<marks> nested;
        };

        class registry {
          private:
            formats::dictionary<marks> m_keys;
            std::vector<key_path> m_deferred;

            static auto _child(formats::dictionary<marks>& level, std::string_view part) -> marks& {
                const auto found = level.find(part);
                return found ? found->get() : level.set(part, marks{});
            }

          public:
            [[nodiscard]] auto has(std::span<const std::string> key, mark which) const -> bool {
                if (key.empty()) {
                    return false;
                }

                const auto bit = std::to_underlying(which);
                std::reference_wrapper<const formats::dictionary<marks>> level{m_keys};

                for (const auto& part : key.first(key.size() - 1)) {
                    const auto found = level.get().find(part);
                    if (!found) {
                        return false;
                    }
                    if ((found->get().inherited & bit) != 0) {
                        return true;
                    }

                    level = found->get().nested;
                }

                const auto found = level.get().find(key.back());
                return found && ((found->get().own | found->get().inherited) & bit) != 0;
            }

            auto set(std::span<const std::string> key, mark which, bool inherited) -> void {
                std::reference_wrapper<formats::dictionary<marks>> level{m_keys};
                for (const auto& part : key.first(key.size() - 1)) {
                    level = _child(level, part).nested;
                }

                auto& target = _child(level, key.back());
                (inherited ? target.inherited : target.own) |= std::to_underlying(which);
            }

            auto reset(std::span<const std::string> key) -> void {
                std::reference_wrapper<formats::dictionary<marks>> level{m_keys};

                for (const auto& part : key.first(key.size() - 1)) {
                    const auto found = level.get().find(part);
                    if (!found) {
                        return;
                    }

                    level = found->get().nested;
                }

                level.get().remove(key.back());
            }

            auto define_later(std::span<const std::string> key) -> void {
                m_deferred.emplace_back(key.begin(), key.end());
            }

            auto settle() -> void {
                for (const auto& key : m_deferred) {
                    set(key, mark::defined, false);
                }

                m_deferred.clear();
            }
        };

        class parser {
          private:
            struct located {
                std::reference_wrapper<toml::table> table;
                std::size_t depth{};
            };

            std::string_view m_text;
            std::size_t m_at{};
            std::uint32_t m_line{1};
            std::size_t m_counted{};
            toml::table m_root;
            registry m_marks;
            key_path m_header;

            [[nodiscard]] auto _rest() const noexcept -> std::string_view { return m_text.substr(m_at); }
            [[nodiscard]] auto _done() const noexcept -> bool { return m_at == m_text.size(); }

            [[nodiscard]] auto _at(char unit) const noexcept -> bool {
                return !_done() && m_text[m_at] == unit;
            }

            [[nodiscard]] auto _fail(std::size_t offset, std::string_view reason,
                                     formats::error_code code = formats::error_code::invalid_syntax) const
                -> std::unexpected<rei::error> {
                const auto before = m_text.substr(0, offset);
                const auto newline = before.rfind('\n');
                const auto start = newline == std::string_view::npos ? 0 : newline + 1;

                const auto line = 1 + std::ranges::count(before, '\n');
                const auto column = 1 + std::ranges::count_if(before.substr(start), [](char unit) -> bool {
                                        return (static_cast<unsigned char>(unit) & 0xC0) != 0x80;
                                    });

                return std::unexpected(rei::error{code, std::format("{}:{}: {}", line, column, reason)});
            }

            [[nodiscard]] auto _too_deep(std::size_t offset) const -> std::unexpected<rei::error> {
                return _fail(offset, std::format("the document nests deeper than {} levels", max_depth),
                             formats::error_code::depth_exceeded);
            }

            auto _line_of(std::size_t offset) noexcept -> std::uint32_t {
                const auto passed = m_text.substr(m_counted, offset - m_counted);
                m_line += static_cast<std::uint32_t>(std::ranges::count(passed, '\n'));
                m_counted = offset;
                return m_line;
            }

            auto _skip_blank() noexcept -> void {
                while (_at(' ') || _at('\t')) {
                    m_at++;
                }
            }

            auto _newline() noexcept -> bool {
                if (_at('\n')) {
                    m_at++;
                    return true;
                }
                if (_rest().starts_with("\r\n")) {
                    m_at += 2;
                    return true;
                }

                return false;
            }

            auto _comment() -> std::expected<void, rei::error> {
                if (!_at('#')) {
                    return {};
                }

                m_at++;
                while (!_done() && !_at('\n') && !_rest().starts_with("\r\n")) {
                    if (is_control(m_text[m_at])) {
                        return _fail(m_at, std::format("control character U+{:04X} in a comment",
                                                       static_cast<unsigned char>(m_text[m_at])));
                    }

                    m_at++;
                }

                return {};
            }

            auto _gaps() -> std::expected<void, rei::error> {
                while (true) {
                    _skip_blank();
                    if (_newline()) {
                        continue;
                    }
                    if (!_at('#')) {
                        return {};
                    }
                    if (auto comment = _comment(); !comment) {
                        return comment;
                    }
                }
            }

            auto _end_of_line(std::string_view after) -> std::expected<void, rei::error> {
                _skip_blank();
                if (auto comment = _comment(); !comment) {
                    return comment;
                }
                if (_done() || _newline()) {
                    return {};
                }

                return _fail(m_at, std::format("expected a newline after {}", after));
            }

            auto _code_point(std::string& out, std::size_t start, std::size_t digits)
                -> std::expected<void, rei::error> {
                const auto hex = m_text.substr(m_at, digits);
                if (hex.size() != digits || !std::ranges::all_of(hex, is_hex_digit)) {
                    return _fail(start, std::format("'{}' must be followed by {} hexadecimal digits",
                                                    m_text.substr(start, 2), digits));
                }

                std::uint32_t scalar{};
                std::from_chars(hex.data(), hex.data() + hex.size(), scalar, 16);
                if (!core::utf8::append(out, static_cast<char32_t>(scalar))) {
                    return _fail(start, std::format("'{}' is not a Unicode scalar value",
                                                    m_text.substr(start, 2 + digits)));
                }

                m_at += digits;
                return {};
            }

            auto _escape(std::string& out) -> std::expected<void, rei::error> {
                const auto start = m_at;
                m_at++;

                if (_done() || is_control(m_text[m_at])) {
                    return _fail(start, "'\\' must be followed by an escape character");
                }

                const auto unit = m_text[m_at];
                m_at++;

                switch (unit) {
                case 'b':
                    out += '\b';
                    return {};
                case 't':
                    out += '\t';
                    return {};
                case 'n':
                    out += '\n';
                    return {};
                case 'f':
                    out += '\f';
                    return {};
                case 'r':
                    out += '\r';
                    return {};
                case 'e':
                    out += '\x1B';
                    return {};
                case '"':
                case '\\':
                    out += unit;
                    return {};
                case 'x':
                    return _code_point(out, start, 2);
                case 'u':
                    return _code_point(out, start, 4);
                case 'U':
                    return _code_point(out, start, 8);
                default:
                    break;
                }

                const auto rest = m_text.substr(start + 1);
                auto length = 1uz;
                while (length < rest.size() && (static_cast<unsigned char>(rest[length]) & 0xC0) == 0x80) {
                    length++;
                }

                return _fail(start, std::format("'\\{}' is not an escape", rest.substr(0, length)));
            }

            [[nodiscard]] auto _control(std::size_t offset) const -> std::unexpected<rei::error> {
                return _fail(offset, std::format("control character U+{:04X} in a string",
                                                 static_cast<unsigned char>(m_text[offset])));
            }

            auto _single_line(char quote) -> std::expected<std::string, rei::error> {
                const auto start = m_at;
                m_at++;
                std::string out;

                while (true) {
                    if (_done() || _at('\n') || _at('\r')) {
                        return _fail(start, "unterminated string");
                    }

                    const auto unit = m_text[m_at];
                    if (unit == quote) {
                        m_at++;
                        return out;
                    }

                    if (unit == '\\' && quote == '"') {
                        if (auto escaped = _escape(out); !escaped) {
                            return failure(escaped);
                        }
                        continue;
                    }

                    if (is_control(unit)) {
                        return _control(m_at);
                    }

                    out += unit;
                    m_at++;
                }
            }

            auto _continuation() noexcept -> bool {
                auto next = m_at + 1;
                while (next < m_text.size() && (m_text[next] == ' ' || m_text[next] == '\t')) {
                    next++;
                }

                const auto rest = m_text.substr(next);
                if (!rest.starts_with('\n') && !rest.starts_with("\r\n")) {
                    return false;
                }

                m_at = next;
                while (_newline() || _at(' ') || _at('\t')) {
                    _skip_blank();
                }

                return true;
            }

            auto _multiline(char quote) -> std::expected<std::string, rei::error> {
                const auto start = m_at;
                m_at += 3;
                _newline();
                std::string out;

                while (true) {
                    if (_done()) {
                        return _fail(start, "unterminated string");
                    }

                    const auto unit = m_text[m_at];
                    if (unit == quote) {
                        const auto rest = _rest();
                        const auto quotes = std::min(rest.find_first_not_of(quote), rest.size());
                        const auto kept = quotes < 3 ? quotes : std::min(quotes - 3, 2uz);

                        out.append(kept, quote);
                        m_at += kept;
                        if (quotes < 3) {
                            continue;
                        }

                        m_at += 3;
                        return out;
                    }

                    if (unit == '\\' && quote == '"') {
                        if (_continuation()) {
                            continue;
                        }
                        if (auto escaped = _escape(out); !escaped) {
                            return failure(escaped);
                        }
                        continue;
                    }

                    if (_newline()) {
                        out += '\n';
                        continue;
                    }
                    if (is_control(unit)) {
                        return _control(m_at);
                    }

                    out += unit;
                    m_at++;
                }
            }

            auto _key_part() -> std::expected<std::string, rei::error> {
                if (_at('"') || _at('\'')) {
                    return _single_line(m_text[m_at]);
                }

                const auto start = m_at;
                while (!_done() && is_bare(m_text[m_at])) {
                    m_at++;
                }

                if (m_at == start) {
                    return _fail(start, "expected a key");
                }

                return std::string{m_text.substr(start, m_at - start)};
            }

            auto _key() -> std::expected<key_path, rei::error> {
                key_path parts;

                while (true) {
                    auto part = _key_part();
                    if (!part) {
                        return failure(part);
                    }

                    parts.push_back(std::move(*part));
                    _skip_blank();
                    if (!_at('.')) {
                        return parts;
                    }

                    m_at++;
                    _skip_blank();
                }
            }

            auto _nest(toml::table& root, std::size_t depth, std::span<const std::string> path,
                       bool through_arrays, std::size_t offset, std::uint32_t line)
                -> std::expected<located, rei::error> {
                located current{.table = root, .depth = depth};

                for (auto i = 0uz; i < path.size(); i++) {
                    auto& table = current.table.get();
                    const auto found = table.find(path[i]);

                    std::reference_wrapper<toml::value> target{found ? found->get()
                                                                     : table.set(path[i], toml::table{})};
                    if (!found) {
                        target.get().line(line);
                    }

                    auto level = current.depth + 1;
                    if (auto items = target.get().edit_array();
                        through_arrays && items && !items->get().empty()) {
                        target = items->get().back();
                        level++;
                    }

                    const auto child = target.get().edit_table();
                    if (!child) {
                        return _fail(offset, std::format("'{}' is not a table", joined(path.first(i + 1))));
                    }
                    if (level > max_depth) {
                        return _too_deep(offset);
                    }

                    current = {.table = *child, .depth = level};
                }

                return current;
            }

            auto _integer(std::string_view token, std::string_view digits, int base, bool negative,
                          std::size_t start) const -> std::expected<toml::value, rei::error> {
                const auto plain = (negative ? "-" : "") + without_underscores(digits);

                std::int64_t number{};
                const auto [end, error] =
                    std::from_chars(plain.data(), plain.data() + plain.size(), number, base);
                if (error != std::errc{}) {
                    return _fail(start, std::format("{} is out of range for an integer", token));
                }

                return toml::value{number};
            }

            auto _number(std::string_view token, std::size_t start) const
                -> std::expected<toml::value, rei::error> {
                const auto invalid = [&]() -> std::unexpected<rei::error> {
                    return _fail(start, std::format("'{}' is not a valid value", token));
                };

                const auto negative = token.starts_with('-');
                const auto body = token.substr(negative || token.starts_with('+') ? 1 : 0);
                const auto sign = negative ? -1.0 : 1.0;

                if (body == "inf") {
                    return toml::value{sign * std::numeric_limits<double>::infinity()};
                }
                if (body == "nan") {
                    return toml::value{sign * std::numeric_limits<double>::quiet_NaN()};
                }

                if (body.size() != token.size() &&
                    (body.starts_with("0x") || body.starts_with("0o") || body.starts_with("0b"))) {
                    return invalid();
                }
                if (body.starts_with("0x")) {
                    return is_run(body.substr(2), is_hex_digit)
                               ? _integer(token, body.substr(2), 16, false, start)
                               : invalid();
                }
                if (body.starts_with("0o")) {
                    return is_run(body.substr(2), is_octal_digit)
                               ? _integer(token, body.substr(2), 8, false, start)
                               : invalid();
                }
                if (body.starts_with("0b")) {
                    return is_run(body.substr(2), is_binary_digit)
                               ? _integer(token, body.substr(2), 2, false, start)
                               : invalid();
                }

                const auto exponent_at = body.find_first_of("eE");
                const auto mantissa = body.substr(0, exponent_at);
                const auto point = mantissa.find('.');
                const auto whole = mantissa.substr(0, point);

                if (!is_run(whole, is_digit) || (whole.size() > 1 && whole.starts_with('0'))) {
                    return invalid();
                }
                if (point == std::string_view::npos && exponent_at == std::string_view::npos) {
                    return _integer(token, whole, 10, negative, start);
                }

                if (point != std::string_view::npos && !is_run(mantissa.substr(point + 1), is_digit)) {
                    return invalid();
                }
                if (exponent_at != std::string_view::npos) {
                    const auto exponent = body.substr(exponent_at + 1);
                    const auto digits =
                        exponent.substr(exponent.starts_with('+') || exponent.starts_with('-') ? 1 : 0);
                    if (!is_run(digits, is_digit)) {
                        return invalid();
                    }
                }

                auto plain = (negative ? "-" : "") + without_underscores(body);
                std::erase(plain, '+');

                double number{};
                const auto [end, error] = std::from_chars(plain.data(), plain.data() + plain.size(), number);
                if (error != std::errc{}) {
                    return _fail(start, std::format("{} is out of range for a float", token));
                }

                return toml::value{number};
            }

            auto _moment(std::string_view token, std::size_t start) const
                -> std::expected<toml::value, rei::error> {
                const auto date = read_date(token.substr(0, 10));
                if (!date) {
                    return _fail(start, std::format("'{}' is not a valid date", token));
                }
                if (token.size() == 10) {
                    return toml::value{*date};
                }

                const auto clock = token.substr(11);
                const auto offset_at = clock.find_first_of("Zz+-");
                const auto time = read_time(clock.substr(0, offset_at));

                const auto separated = token[10] == 'T' || token[10] == 't' || token[10] == ' ';
                const auto offset = offset_at == std::string_view::npos
                                        ? std::optional<std::chrono::minutes>{}
                                        : read_offset(clock.substr(offset_at));

                if (!separated || !time || (offset_at != std::string_view::npos && !offset)) {
                    return _fail(start, std::format("'{}' is not a valid date-time", token));
                }

                return toml::value{toml::date_time{.date = *date, .time = *time, .offset = offset}};
            }

            auto _scalar() -> std::expected<toml::value, rei::error> {
                const auto start = m_at;
                const auto scan = [&]() -> std::string_view {
                    while (!_done() && is_scalar(m_text[m_at])) {
                        m_at++;
                    }

                    return m_text.substr(start, m_at - start);
                };

                auto token = scan();
                if (token.empty()) {
                    return _fail(start, "expected a value");
                }

                if (token == "true" || token == "false") {
                    return toml::value{token == "true"};
                }

                const auto dated = token.size() >= 10 && token[4] == '-' && is_digit(token[0]);
                const auto rest = _rest();
                if (dated && token.size() == 10 && rest.size() >= 4 && rest[0] == ' ' && is_digit(rest[1]) &&
                    is_digit(rest[2]) && rest[3] == ':') {
                    m_at++;
                    token = scan();
                }

                if (dated) {
                    return _moment(token, start);
                }

                if (token.size() >= 3 && is_digit(token[0]) && is_digit(token[1]) && token[2] == ':') {
                    const auto time = read_time(token);
                    if (!time) {
                        return _fail(start, std::format("'{}' is not a valid time", token));
                    }

                    return toml::value{*time};
                }

                return _number(token, start);
            }

            auto _array(std::size_t depth) -> std::expected<toml::value, rei::error> {
                if (depth > max_depth) {
                    return _too_deep(m_at);
                }

                m_at++;
                toml::array items;

                while (true) {
                    if (auto gap = _gaps(); !gap) {
                        return failure(gap);
                    }
                    if (_at(']')) {
                        break;
                    }

                    auto item = _value(depth + 1);
                    if (!item) {
                        return item;
                    }

                    items.push_back(std::move(*item));
                    if (auto gap = _gaps(); !gap) {
                        return failure(gap);
                    }

                    if (_at(',')) {
                        m_at++;
                    } else if (!_at(']')) {
                        return _fail(m_at, "expected ',' or ']' in an array");
                    }
                }

                m_at++;
                return toml::value{std::move(items)};
            }

            auto _inline_table(std::size_t depth) -> std::expected<toml::value, rei::error> {
                if (depth > max_depth) {
                    return _too_deep(m_at);
                }

                m_at++;
                toml::table table;
                registry marks;

                while (true) {
                    if (auto gap = _gaps(); !gap) {
                        return failure(gap);
                    }
                    if (_at('}')) {
                        break;
                    }

                    const auto start = m_at;
                    const auto line = _line_of(start);

                    auto key = _key();
                    if (!key) {
                        return failure(key);
                    }
                    if (!_at('=')) {
                        return _fail(m_at, "expected '=' after a key");
                    }

                    m_at++;
                    _skip_blank();

                    const std::span<const std::string> name{*key};
                    if (marks.has(name, mark::closed)) {
                        return _fail(start, std::format("'{}' was defined inline and can't be extended",
                                                        joined(name.first(name.size() - 1))));
                    }

                    auto nest = _nest(table, depth, name.first(name.size() - 1), false, start, line);
                    if (!nest) {
                        return failure(nest);
                    }
                    if (nest->table.get().contains(name.back())) {
                        return _fail(start, std::format("key '{}' is defined twice", joined(name)));
                    }

                    auto value = _value(nest->depth + 1);
                    if (!value) {
                        return value;
                    }

                    if (value->kind() == kind::array || value->kind() == kind::table) {
                        marks.set(name, mark::closed, true);
                    }

                    nest->table.get().set(name.back(), std::move(*value));
                    if (auto gap = _gaps(); !gap) {
                        return failure(gap);
                    }

                    if (_at(',')) {
                        m_at++;
                    } else if (!_at('}')) {
                        return _fail(m_at, "expected ',' or '}' in an inline table");
                    }
                }

                m_at++;
                return toml::value{std::move(table)};
            }

            auto _unpositioned(std::size_t depth) -> std::expected<toml::value, rei::error> {
                const auto rest = _rest();

                if (rest.starts_with(R"(""")") || rest.starts_with("'''")) {
                    return _multiline(rest.front());
                }
                if (rest.starts_with('"') || rest.starts_with('\'')) {
                    return _single_line(rest.front());
                }
                if (rest.starts_with('[')) {
                    return _array(depth);
                }
                if (rest.starts_with('{')) {
                    return _inline_table(depth);
                }

                return _scalar();
            }

            auto _value(std::size_t depth) -> std::expected<toml::value, rei::error> {
                const auto line = _line_of(m_at);

                auto value = _unpositioned(depth);
                if (value) {
                    value->line(line);
                }

                return value;
            }

            auto _pair() -> std::expected<void, rei::error> {
                const auto start = m_at;
                const auto line = _line_of(start);

                auto key = _key();
                if (!key) {
                    return failure(key);
                }
                if (!_at('=')) {
                    return _fail(m_at, "expected '=' after a key");
                }

                m_at++;
                _skip_blank();

                auto path = m_header;
                path.insert(path.end(), key->begin(), key->end());
                const std::span<const std::string> name{path};
                const auto parent = name.first(name.size() - 1);

                for (auto i = m_header.size() + 1; i < name.size(); i++) {
                    if (m_marks.has(name.first(i), mark::defined)) {
                        return _fail(start, std::format("table '{}' is already defined and can't be extended "
                                                        "with a dotted key",
                                                        joined(name.first(i))));
                    }

                    m_marks.define_later(name.first(i));
                }

                if (m_marks.has(parent, mark::closed)) {
                    return _fail(
                        start, std::format("'{}' was defined inline and can't be extended", joined(parent)));
                }

                auto nest = _nest(m_root, 0, parent, true, start, line);
                if (!nest) {
                    return failure(nest);
                }
                if (nest->table.get().contains(name.back())) {
                    return _fail(start, std::format("key '{}' is defined twice", joined(name)));
                }

                auto value = _value(nest->depth + 1);
                if (!value) {
                    return failure(value);
                }

                if (value->kind() == kind::array || value->kind() == kind::table) {
                    m_marks.set(name, mark::closed, true);
                }

                nest->table.get().set(name.back(), std::move(*value));
                return {};
            }

            auto _header() -> std::expected<void, rei::error> {
                const auto start = m_at;
                const auto line = _line_of(start);
                const std::string_view closing = _rest().starts_with("[[") ? "]]" : "]";

                m_at += closing.size();
                _skip_blank();

                auto key = _key();
                if (!key) {
                    return failure(key);
                }
                if (!_rest().starts_with(closing)) {
                    return _fail(m_at, std::format("expected '{}' after a table name", closing));
                }

                m_at += closing.size();
                m_marks.settle();

                const std::span<const std::string> name{*key};
                if (m_marks.has(name, mark::closed)) {
                    return _fail(start,
                                 std::format("'{}' was defined inline and can't be extended", joined(name)));
                }

                if (closing == "]") {
                    if (m_marks.has(name, mark::defined)) {
                        return _fail(start, std::format("table '{}' is defined twice", joined(name)));
                    }

                    m_marks.set(name, mark::defined, false);
                    if (auto nest = _nest(m_root, 0, name, true, start, line); !nest) {
                        return failure(nest);
                    }
                } else {
                    m_marks.reset(name);
                    m_marks.set(name, mark::defined, false);

                    auto parent = _nest(m_root, 0, name.first(name.size() - 1), true, start, line);
                    if (!parent) {
                        return failure(parent);
                    }
                    if (parent->depth + 2 > max_depth) {
                        return _too_deep(start);
                    }

                    toml::value element{toml::table{}};
                    element.line(line);

                    auto& table = parent->table.get();
                    if (const auto existing = table.find(name.back())) {
                        const auto items = existing->get().edit_array();
                        if (!items) {
                            return _fail(start, std::format("'{}' is not an array of tables", joined(name)));
                        }

                        items->get().push_back(std::move(element));
                    } else {
                        toml::array items;
                        items.push_back(std::move(element));
                        table.set(name.back(), std::move(items)).line(line);
                    }
                }

                m_header = std::move(*key);
                return {};
            }

          public:
            explicit parser(std::string_view text)
                : m_text(text.substr(text.starts_with(byte_order_mark) ? byte_order_mark.size() : 0)) {}

            auto run() -> std::expected<toml::table, rei::error> {
                if (const auto valid = core::utf8::valid_prefix(m_text); valid.size() != m_text.size()) {
                    return _fail(valid.size(), "invalid UTF-8");
                }

                while (true) {
                    _skip_blank();
                    if (_done()) {
                        break;
                    }
                    if (_newline()) {
                        continue;
                    }

                    if (_at('#')) {
                        if (auto comment = _comment(); !comment) {
                            return failure(comment);
                        }
                        continue;
                    }

                    const auto header = _at('[');
                    if (auto read = header ? _header() : _pair(); !read) {
                        return failure(read);
                    }
                    if (auto ended = _end_of_line(header ? "a table header" : "a value"); !ended) {
                        return failure(ended);
                    }
                }

                return std::move(m_root);
            }
        };
    } // namespace

    auto parse(std::string_view text) -> std::expected<toml::table, rei::error> {
        return parser{text}.run();
    }
} // namespace rei::formats::toml
