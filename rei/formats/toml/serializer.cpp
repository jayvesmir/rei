#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "rei/core/error.hpp"
#include "rei/core/utf8.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/toml.hpp"
#include "rei/formats/toml/detail.hpp"

namespace rei::formats::toml {
    namespace {
        using step = std::variant<std::string_view, std::size_t>;

        auto is_sectioned(const toml::value& value) noexcept -> bool {
            if (value.kind() == kind::table) {
                return true;
            }

            const auto items = value.as_array();
            return items && !items->empty() &&
                   std::ranges::all_of(
                       *items, [](const toml::value& item) -> bool { return item.kind() == kind::table; });
        }

        auto has_heading(const toml::table& table) noexcept -> bool {
            return table.empty() || !std::ranges::all_of(table, [](const toml::table::entry& entry) -> bool {
                       return is_sectioned(entry.value);
                   });
        }

        class writer {
          private:
            std::string m_out;
            std::vector<step> m_trail;

            [[nodiscard]] auto _fail(std::string_view reason) const -> std::unexpected<rei::error> {
                std::string where;

                for (const auto& part : m_trail) {
                    if (std::holds_alternative<std::size_t>(part)) {
                        where += std::format("[{}]", std::get<std::size_t>(part));
                    } else {
                        where += where.empty() ? "" : ".";
                        where += std::get<std::string_view>(part);
                    }
                }

                return std::unexpected(rei::error{
                    formats::error_code::invalid_value,
                    where.empty() ? std::string{reason} : std::format("{}: {}", where, reason),
                });
            }

            static auto _quoted(std::string& out, std::string_view text) -> void {
                out += '"';

                for (const auto unit : text) {
                    const auto byte = static_cast<unsigned char>(unit);

                    switch (unit) {
                    case '"':
                        out += R"(\")";
                        break;
                    case '\\':
                        out += R"(\\)";
                        break;
                    case '\b':
                        out += R"(\b)";
                        break;
                    case '\t':
                        out += R"(\t)";
                        break;
                    case '\n':
                        out += R"(\n)";
                        break;
                    case '\f':
                        out += R"(\f)";
                        break;
                    case '\r':
                        out += R"(\r)";
                        break;
                    default:
                        if (byte < 0x20 || byte == 0x7F) {
                            std::format_to(std::back_inserter(out), R"(\u{:04X})", byte);
                        } else {
                            out += unit;
                        }
                    }
                }

                out += '"';
            }

            auto _key(std::string& out, std::string_view key) const -> std::expected<void, rei::error> {
                if (!key.empty() && std::ranges::all_of(key, detail::is_bare)) {
                    out += key;
                    return {};
                }

                if (!core::utf8::is_valid(key)) {
                    return _fail("a key is not valid UTF-8");
                }

                _quoted(out, key);
                return {};
            }

            auto _floating(double number) -> void {
                if (std::isnan(number)) {
                    m_out += "nan";
                    return;
                }
                if (std::isinf(number)) {
                    m_out += number < 0 ? "-inf" : "inf";
                    return;
                }

                std::array<char, 32> buffer{};
                const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
                const std::string_view text{buffer.data(), end};

                m_out += text;
                if (text.find_first_of(".e") == std::string_view::npos) {
                    m_out += ".0";
                }
            }

            auto _date(const toml::date& date) -> std::expected<void, rei::error> {
                const auto year = static_cast<int>(date.year());
                const auto month = static_cast<unsigned>(date.month());
                const auto day = static_cast<unsigned>(date.day());

                if (!date.ok() || year < 0 || year > 9999) {
                    return _fail(std::format("{}-{}-{} is not a date", year, month, day));
                }

                std::format_to(std::back_inserter(m_out), "{:04}-{:02}-{:02}", year, month, day);
                return {};
            }

            auto _time(const toml::time& time) -> std::expected<void, rei::error> {
                if (time.hour > 23 || time.minute > 59 || time.second > 59 || time.nanosecond > 999'999'999) {
                    return _fail(std::format("{}:{}:{}.{} is not a time", time.hour, time.minute, time.second,
                                             time.nanosecond));
                }

                std::format_to(std::back_inserter(m_out), "{:02}:{:02}:{:02}", time.hour, time.minute,
                               time.second);
                if (time.nanosecond == 0) {
                    return {};
                }

                const auto fraction = std::format("{:09}", time.nanosecond);
                m_out += '.';
                m_out += std::string_view{fraction}.substr(0, fraction.find_last_not_of('0') + 1);
                return {};
            }

            auto _moment(const toml::date_time& moment) -> std::expected<void, rei::error> {
                if (auto date = _date(moment.date); !date) {
                    return date;
                }

                m_out += 'T';
                if (auto time = _time(moment.time); !time) {
                    return time;
                }

                if (!moment.offset) {
                    return {};
                }

                const auto minutes = moment.offset->count();
                const auto distance = minutes < 0 ? -minutes : minutes;
                if (distance >= 24 * 60) {
                    return _fail(std::format("an offset of {} minutes is out of range", minutes));
                }

                if (minutes == 0) {
                    m_out += 'Z';
                } else {
                    std::format_to(std::back_inserter(m_out), "{}{:02}:{:02}", minutes < 0 ? '-' : '+',
                                   distance / 60, distance % 60);
                }

                return {};
            }

            auto _array(std::span<const toml::value> items) -> std::expected<void, rei::error> {
                m_out += '[';

                for (auto i = 0uz; i < items.size(); i++) {
                    m_out += i == 0 ? "" : ", ";
                    m_trail.emplace_back(i);

                    if (auto item = _inline(items[i]); !item) {
                        return item;
                    }

                    m_trail.pop_back();
                }

                m_out += ']';
                return {};
            }

            auto _inline_table(const toml::table& table) -> std::expected<void, rei::error> {
                if (table.empty()) {
                    m_out += "{}";
                    return {};
                }

                m_out += "{ ";

                for (auto first = true; const auto& [key, value] : table) {
                    m_out += first ? "" : ", ";
                    first = false;

                    if (auto pair = _pair(key, value); !pair) {
                        return pair;
                    }
                }

                m_out += " }";
                return {};
            }

            auto _inline(const toml::value& value) -> std::expected<void, rei::error> {
                switch (value.kind()) {
                case kind::string:
                    if (!core::utf8::is_valid(*value.as_string())) {
                        return _fail("the string is not valid UTF-8");
                    }

                    _quoted(m_out, *value.as_string());
                    return {};
                case kind::integer:
                    std::format_to(std::back_inserter(m_out), "{}", *value.as_integer());
                    return {};
                case kind::floating:
                    _floating(*value.as_floating());
                    return {};
                case kind::boolean:
                    m_out += *value.as_boolean() ? "true" : "false";
                    return {};
                case kind::date:
                    return _date(*value.as_date());
                case kind::time:
                    return _time(*value.as_time());
                case kind::date_time:
                    return _moment(*value.as_date_time());
                case kind::array:
                    return _array(*value.as_array());
                case kind::table:
                    return _inline_table(*value.as_table());
                }

                std::unreachable();
            }

            auto _pair(std::string_view key, const toml::value& value) -> std::expected<void, rei::error> {
                if (auto name = _key(m_out, key); !name) {
                    return name;
                }

                m_out += " = ";
                m_trail.emplace_back(key);

                if (auto written = _inline(value); !written) {
                    return written;
                }

                m_trail.pop_back();
                return {};
            }

            auto _heading(std::string_view opening, std::string_view name, std::string_view closing) -> void {
                m_out += m_out.empty() ? "" : "\n";
                m_out += opening;
                m_out += name;
                m_out += closing;
                m_out += '\n';
            }

            auto _name(std::string_view parent, std::string_view key) const
                -> std::expected<std::string, rei::error> {
                std::string name{parent};
                name += parent.empty() ? "" : ".";

                if (auto written = _key(name, key); !written) {
                    return std::unexpected(std::move(written).error());
                }

                return name;
            }

            auto _table(const toml::table& table, std::string_view name) -> std::expected<void, rei::error> {
                for (const auto& [key, value] : table) {
                    if (is_sectioned(value)) {
                        continue;
                    }
                    if (auto pair = _pair(key, value); !pair) {
                        return pair;
                    }

                    m_out += '\n';
                }

                for (const auto& [key, value] : table) {
                    if (!is_sectioned(value)) {
                        continue;
                    }

                    const auto child = _name(name, key);
                    if (!child) {
                        return std::unexpected(child.error());
                    }

                    m_trail.emplace_back(key);

                    if (const auto entries = value.as_table()) {
                        if (has_heading(*entries)) {
                            _heading("[", *child, "]");
                        }
                        if (auto section = _table(*entries, *child); !section) {
                            return section;
                        }
                    } else {
                        const auto items = *value.as_array();

                        for (auto i = 0uz; i < items.size(); i++) {
                            _heading("[[", *child, "]]");
                            m_trail.emplace_back(i);

                            if (auto section = _table(*items[i].as_table(), *child); !section) {
                                return section;
                            }

                            m_trail.pop_back();
                        }
                    }

                    m_trail.pop_back();
                }

                return {};
            }

          public:
            auto document(const toml::table& table) -> std::expected<std::string, rei::error> {
                if (auto written = _table(table, {}); !written) {
                    return std::unexpected(std::move(written).error());
                }

                return std::move(m_out);
            }

            auto single(const toml::value& value) -> std::expected<std::string, rei::error> {
                if (auto written = _inline(value); !written) {
                    return std::unexpected(std::move(written).error());
                }

                return std::move(m_out);
            }
        };
    } // namespace

    auto serialize(const toml::table& document) -> std::expected<std::string, rei::error> {
        return writer{}.document(document);
    }

    auto serialize(const toml::value& value) -> std::expected<std::string, rei::error> {
        return writer{}.single(value);
    }
} // namespace rei::formats::toml
