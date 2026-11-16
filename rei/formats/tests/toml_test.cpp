#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/formats/toml.hpp"

namespace rei::formats::tests {
    namespace {
        using nlohmann::json;

        template <typename Number> auto number(std::string_view text) -> std::optional<Number> {
            Number value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size() ? std::optional{value}
                                                                            : std::nullopt;
        }

        auto expected_date(std::string_view text) -> std::optional<toml::date> {
            if (text.size() != 10) {
                return std::nullopt;
            }

            const auto year = number<int>(text.substr(0, 4));
            const auto month = number<unsigned>(text.substr(5, 2));
            const auto day = number<unsigned>(text.substr(8, 2));
            if (!year || !month || !day) {
                return std::nullopt;
            }

            return toml::date{std::chrono::year{*year}, std::chrono::month{*month}, std::chrono::day{*day}};
        }

        auto expected_time(std::string_view text) -> std::optional<toml::time> {
            if (text.size() < 8) {
                return std::nullopt;
            }

            const auto hour = number<std::uint8_t>(text.substr(0, 2));
            const auto minute = number<std::uint8_t>(text.substr(3, 2));
            const auto second = number<std::uint8_t>(text.substr(6, 2));
            if (!hour || !minute || !second) {
                return std::nullopt;
            }

            std::string fraction{text.size() > 9 ? text.substr(9) : std::string_view{}};
            fraction.resize(9, '0');
            const auto nanosecond = number<std::uint32_t>(fraction);
            if (!nanosecond) {
                return std::nullopt;
            }

            return toml::time{.hour = *hour, .minute = *minute, .second = *second, .nanosecond = *nanosecond};
        }

        auto expected_moment(std::string_view text) -> std::optional<toml::date_time> {
            if (text.size() < 19 || text[10] != 'T') {
                return std::nullopt;
            }

            const auto offset_at = text.find_first_of("Z+-", 11);
            const auto date = expected_date(text.substr(0, 10));
            const auto time = expected_time(text.substr(11, offset_at - 11));
            if (!date || !time) {
                return std::nullopt;
            }

            toml::date_time moment{.date = *date, .time = *time};
            if (offset_at == std::string_view::npos) {
                return moment;
            }

            const auto zone = text.substr(offset_at);
            if (zone == "Z") {
                moment.offset = std::chrono::minutes{0};
                return moment;
            }

            if (zone.size() != 6) {
                return std::nullopt;
            }

            const auto hours = number<int>(zone.substr(1, 2));
            const auto minutes = number<int>(zone.substr(4, 2));
            if (!hours || !minutes) {
                return std::nullopt;
            }

            const std::chrono::minutes offset{(*hours * 60) + *minutes};
            moment.offset = zone.starts_with('-') ? -offset : offset;
            return moment;
        }

        auto matches(const toml::table& actual, const json& expected) -> bool;

        auto matches_scalar(const toml::value& actual, std::string_view type, std::string_view text) -> bool {
            if (type == "string") {
                return actual.as_string() == text;
            }
            if (type == "integer") {
                return actual.as_integer() && actual.as_integer() == number<std::int64_t>(text);
            }
            if (type == "bool") {
                return actual.as_boolean() == (text == "true");
            }
            if (type == "date-local") {
                return actual.as_date() && actual.as_date() == expected_date(text);
            }
            if (type == "time-local") {
                return actual.as_time() && actual.as_time() == expected_time(text);
            }
            if (type == "datetime" || type == "datetime-local") {
                const auto moment = actual.as_date_time();
                return moment && moment == expected_moment(text) &&
                       moment->offset.has_value() == (type == "datetime");
            }
            if (type != "float" || actual.kind() != toml::kind::floating) {
                return false;
            }

            const auto held = *actual.as_floating();
            if (text.ends_with("nan")) {
                return std::isnan(held);
            }
            if (text.ends_with("inf")) {
                return std::isinf(held) && std::signbit(held) == text.starts_with('-');
            }

            return held == number<double>(text.substr(text.starts_with('+') ? 1 : 0));
        }

        auto matches(const toml::value& actual, const json& expected) -> bool {
            if (expected.is_array()) {
                const auto items = actual.as_array();
                if (!items || items->size() != expected.size()) {
                    return false;
                }

                for (auto i = 0uz; i < items->size(); i++) {
                    if (!matches((*items)[i], expected[i])) {
                        return false;
                    }
                }

                return true;
            }

            const auto tagged = expected.size() == 2 && expected.contains("type") &&
                                expected.contains("value") && expected["type"].is_string() &&
                                expected["value"].is_string();
            if (tagged) {
                return matches_scalar(actual, expected["type"].get_ref<const std::string&>(),
                                      expected["value"].get_ref<const std::string&>());
            }

            const auto table = actual.as_table();
            return table && matches(*table, expected);
        }

        auto matches(const toml::table& actual, const json& expected) -> bool {
            if (!expected.is_object() || actual.size() != expected.size()) {
                return false;
            }

            return std::ranges::all_of(expected.items(), [&](const auto& item) -> bool {
                const auto found = actual.find(item.key());
                return found && matches(*found, item.value());
            });
        }

        auto built(const json& expected) -> std::optional<toml::value>;

        auto built_scalar(std::string_view type, std::string_view text) -> std::optional<toml::value> {
            const auto wrap = [](const auto& held) -> std::optional<toml::value> {
                return held ? std::optional{toml::value{*held}} : std::nullopt;
            };

            if (type == "string") {
                return toml::value{text};
            }
            if (type == "bool") {
                return toml::value{text == "true"};
            }
            if (type == "integer") {
                return wrap(number<std::int64_t>(text));
            }
            if (type == "date-local") {
                return wrap(expected_date(text));
            }
            if (type == "time-local") {
                return wrap(expected_time(text));
            }
            if (type == "datetime" || type == "datetime-local") {
                return wrap(expected_moment(text));
            }
            if (type != "float") {
                return std::nullopt;
            }

            if (text.ends_with("nan")) {
                return toml::value{std::numeric_limits<double>::quiet_NaN()};
            }
            if (text.ends_with("inf")) {
                const auto infinity = std::numeric_limits<double>::infinity();
                return toml::value{text.starts_with('-') ? -infinity : infinity};
            }

            return wrap(number<double>(text.substr(text.starts_with('+') ? 1 : 0)));
        }

        auto built(const json& expected) -> std::optional<toml::value> {
            if (expected.is_array()) {
                toml::array items;

                for (const auto& element : expected) {
                    auto item = built(element);
                    if (!item) {
                        return std::nullopt;
                    }

                    items.push_back(std::move(*item));
                }

                return toml::value{std::move(items)};
            }

            const auto tagged = expected.size() == 2 && expected.contains("type") &&
                                expected.contains("value") && expected["type"].is_string() &&
                                expected["value"].is_string();
            if (tagged) {
                return built_scalar(expected["type"].get_ref<const std::string&>(),
                                    expected["value"].get_ref<const std::string&>());
            }

            toml::table table;
            for (const auto& item : expected.items()) {
                auto value = built(item.value());
                if (!value) {
                    return std::nullopt;
                }

                table.set(item.key(), std::move(*value));
            }

            return toml::value{std::move(table)};
        }

        auto read(const std::filesystem::path& path) -> std::string {
            std::ifstream file{path, std::ios::binary};
            return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        auto listed(std::string_view directory) -> std::vector<std::filesystem::path> {
            const char* list = std::getenv("TOML_TEST_FILES");
            if (list == nullptr) {
                return {};
            }

            std::vector<std::filesystem::path> cases;
            std::ifstream file{list};

            for (std::string line; std::getline(file, line);) {
                if (line.starts_with(directory) && line.ends_with(".toml")) {
                    cases.push_back(std::filesystem::path{list}.parent_path() / line);
                }
            }

            return cases;
        }
    } // namespace

    TEST(toml_test, parses_every_valid_document) {
        const auto cases = tests::listed("valid/");
        ASSERT_GT(cases.size(), 200u) << "TOML_TEST_FILES should point to toml-test's tests/files-toml-1.1.0";

        for (const auto& path : cases) {
            auto expected_path = path;
            const auto expected =
                json::parse(tests::read(expected_path.replace_extension(".json")), nullptr, false);
            ASSERT_FALSE(expected.is_discarded()) << expected_path;

            const auto document = toml::parse(tests::read(path));
            if (!document) {
                ADD_FAILURE() << path << "\n  " << document.error();
                continue;
            }

            EXPECT_TRUE(tests::matches(*document, expected)) << path;
        }
    }

    TEST(toml_test, writes_every_valid_document_back) {
        const auto cases = tests::listed("valid/");
        ASSERT_GT(cases.size(), 200u) << "TOML_TEST_FILES should point to toml-test's tests/files-toml-1.1.0";

        for (auto path : cases) {
            const auto expected = json::parse(tests::read(path.replace_extension(".json")), nullptr, false);
            ASSERT_FALSE(expected.is_discarded()) << path;

            const auto built = tests::built(expected);
            ASSERT_TRUE(built) << path;

            const auto document = built->as_table();
            ASSERT_TRUE(document) << path;

            const auto text = toml::serialize(document->get());
            if (!text) {
                ADD_FAILURE() << path << "\n  " << text.error();
                continue;
            }

            const auto read_back = toml::parse(*text);
            if (!read_back) {
                ADD_FAILURE() << path << "\n  " << read_back.error() << "\n" << *text;
                continue;
            }

            EXPECT_TRUE(tests::matches(*read_back, expected)) << path << "\n" << *text;
        }
    }

    TEST(toml_test, rejects_every_invalid_document) {
        const auto cases = tests::listed("invalid/");
        ASSERT_GT(cases.size(), 400u) << "TOML_TEST_FILES should point to toml-test's tests/files-toml-1.1.0";

        for (const auto& path : cases) {
            EXPECT_FALSE(toml::parse(tests::read(path))) << path;
        }
    }
} // namespace rei::formats::tests
