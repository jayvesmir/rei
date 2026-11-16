#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/toml.hpp"

namespace rei::formats::tests {
    namespace {
        using namespace std::chrono_literals;

        static_assert(std::constructible_from<toml::value, const char*>);
        static_assert(std::constructible_from<toml::value, std::uint32_t>);
        static_assert(!std::constructible_from<toml::value, std::uint64_t>);
        static_assert(!std::constructible_from<toml::value, char>);
        static_assert(!std::constructible_from<toml::value, std::nullptr_t>);
        static_assert(!std::default_initializable<toml::value>);

        auto keys(const toml::table& table) -> std::vector<std::string> {
            return std::ranges::to<std::vector>(table | std::views::transform(&toml::table::entry::key));
        }

        auto answering(const toml::value& value) -> std::vector<std::string_view> {
            std::vector<std::string_view> accessors;

            if (value.as_string()) {
                accessors.emplace_back("string");
            }
            if (value.as_integer()) {
                accessors.emplace_back("integer");
            }
            if (value.as_floating()) {
                accessors.emplace_back("floating");
            }
            if (value.as_boolean()) {
                accessors.emplace_back("boolean");
            }
            if (value.as_date()) {
                accessors.emplace_back("date");
            }
            if (value.as_time()) {
                accessors.emplace_back("time");
            }
            if (value.as_date_time()) {
                accessors.emplace_back("date_time");
            }
            if (value.as_array()) {
                accessors.emplace_back("array");
            }
            if (value.as_table()) {
                accessors.emplace_back("table");
            }

            return accessors;
        }
    } // namespace

    TEST(formats_dictionary, keeps_unique_keys_in_the_order_they_were_added) {
        toml::table table{{"b", 1}, {"a", 2}, {"b", 3}};
        EXPECT_EQ(keys(table), (std::vector<std::string>{"b", "a"}));
        EXPECT_EQ(table.find("b").and_then(&toml::value::as_integer), 3);

        table.set("c", "text");
        table.set("a", true);
        EXPECT_EQ(keys(table), (std::vector<std::string>{"b", "a", "c"}));
        EXPECT_EQ(table.find("a").and_then(&toml::value::as_boolean), true);

        EXPECT_TRUE(table.remove("b"));
        EXPECT_FALSE(table.remove("b"));
        EXPECT_FALSE(table.contains("b"));
        EXPECT_EQ(keys(table), (std::vector<std::string>{"a", "c"}));
        EXPECT_EQ(table.size(), 2u);
    }

    TEST(formats_dictionary, stays_consistent_past_the_size_where_it_is_indexed) {
        toml::table table;
        for (auto i = 0; i < 1000; i++) {
            table.set(std::format("key_{}", i), i);
        }

        ASSERT_EQ(table.size(), 1000u);
        EXPECT_EQ(table.find("key_0").and_then(&toml::value::as_integer), 0);
        EXPECT_EQ(table.find("key_999").and_then(&toml::value::as_integer), 999);
        EXPECT_FALSE(table.contains("key_1000"));

        table.set("key_500", "replaced");
        EXPECT_EQ(table.size(), 1000u);
        EXPECT_EQ(table.find("key_500").and_then(&toml::value::as_string), "replaced");

        auto copy = table;
        for (auto i = 0; i < 990; i++) {
            EXPECT_TRUE(copy.remove(std::format("key_{}", i)));
        }

        EXPECT_EQ(keys(copy),
                  (std::vector<std::string>{"key_990", "key_991", "key_992", "key_993", "key_994", "key_995",
                                            "key_996", "key_997", "key_998", "key_999"}));
        EXPECT_FALSE(copy.contains("key_500"));
        EXPECT_EQ(copy.find("key_995").and_then(&toml::value::as_integer), 995);

        EXPECT_EQ(table.size(), 1000u);
        EXPECT_TRUE(table.contains("key_17"));
        EXPECT_NE(table, copy);

        copy = table;
        EXPECT_EQ(table, copy);
        copy.set("key_1000", 1000);
        EXPECT_NE(table, copy);

        const auto moved = std::move(copy);
        EXPECT_EQ(moved.size(), 1001u);
        EXPECT_EQ(moved.find("key_1000").and_then(&toml::value::as_integer), 1000);
    }

    TEST(formats_dictionary, finds_through_nested_tables) {
        const toml::table document{
            {"title", "fileserver"},
            {"http", toml::table{{"port", 8443}, {"tls", toml::table{{"min_version", "1.3"}}}}},
        };

        EXPECT_EQ(document.find("http", "port").and_then(&toml::value::as_integer), 8443);
        EXPECT_EQ(document.find("http", "tls", "min_version").and_then(&toml::value::as_string), "1.3");
        EXPECT_FALSE(document.find("http", "prot"));
        EXPECT_FALSE(document.find("title", "length"));
        EXPECT_FALSE(document.find("http", "port", "low"));
    }

    TEST(formats_dictionary, equality_ignores_order_and_lines) {
        const toml::table written{
            {"title", "fileserver"},
            {"http", toml::table{{"host", "::1"}, {"port", 8443}}},
            {"files", toml::array{"a.html", "b.html"}},
        };
        toml::table reordered{
            {"files", toml::array{"a.html", "b.html"}},
            {"http", toml::table{{"port", 8443}, {"host", "::1"}}},
            {"title", "fileserver"},
        };
        EXPECT_EQ(written, reordered);

        toml::value port{8443};
        port.line(4);
        EXPECT_EQ(port.line(), 4u);
        EXPECT_EQ(toml::value{8443}.line(), std::nullopt);
        EXPECT_EQ(port, toml::value{8443});

        reordered.set("files", toml::array{"b.html", "a.html"});
        EXPECT_NE(written, reordered);

        EXPECT_NE(toml::table({{"port", 8443}}), toml::table({{"port", 8443.0}}));
        EXPECT_NE(toml::table({{"port", 8443}}), toml::table({{"port", 8443}, {"host", "::1"}}));
        EXPECT_NE(toml::table({{"port", 8443}}), toml::table({{"prot", 8443}}));
    }

    TEST(toml_value, each_kind_answers_its_own_accessor) {
        const toml::date_time local{.date = 1979y / 5 / 27, .time = {.hour = 7, .minute = 32}};
        const toml::date_time offset{
            .date = 1979y / 5 / 27,
            .time = {.hour = 7, .minute = 32},
            .offset = -7h,
        };

        const toml::value text{"text"};
        EXPECT_EQ(text.kind(), toml::kind::string);
        EXPECT_EQ(answering(text), (std::vector<std::string_view>{"string"}));
        EXPECT_EQ(text.as_string(), "text");
        EXPECT_EQ(toml::value{std::string{"owned"}}.as_string(), "owned");
        EXPECT_EQ(toml::value{std::string_view{"viewed"}}.as_string(), "viewed");

        const toml::value flag{true};
        EXPECT_EQ(flag.kind(), toml::kind::boolean);
        EXPECT_EQ(answering(flag), (std::vector<std::string_view>{"boolean"}));

        const toml::value ratio{1.5F};
        EXPECT_EQ(ratio.kind(), toml::kind::floating);
        EXPECT_EQ(answering(ratio), (std::vector<std::string_view>{"floating"}));
        EXPECT_EQ(ratio.as_floating(), 1.5);

        const toml::value day{toml::date{1979y / 5 / 27}};
        EXPECT_EQ(day.kind(), toml::kind::date);
        EXPECT_EQ(answering(day), (std::vector<std::string_view>{"date"}));
        EXPECT_EQ(day.as_date(), 1979y / 5 / 27);

        const toml::value hour{toml::time{.hour = 7, .minute = 32, .nanosecond = 999'999'000}};
        EXPECT_EQ(hour.kind(), toml::kind::time);
        EXPECT_EQ(answering(hour), (std::vector<std::string_view>{"time"}));
        EXPECT_EQ(hour.as_time(), (toml::time{.hour = 7, .minute = 32, .nanosecond = 999'999'000}));

        const toml::value moment{offset};
        EXPECT_EQ(moment.kind(), toml::kind::date_time);
        EXPECT_EQ(answering(moment), (std::vector<std::string_view>{"date_time"}));
        EXPECT_EQ(moment.as_date_time(), offset);
        EXPECT_NE(moment, toml::value{local});

        const toml::value items{toml::array{1, "two", 3.0}};
        EXPECT_EQ(items.kind(), toml::kind::array);
        EXPECT_EQ(answering(items), (std::vector<std::string_view>{"array"}));
        EXPECT_EQ(items.as_array().transform(&std::span<const toml::value>::size), 3u);

        const toml::value entries{toml::table{{"port", 8443}}};
        EXPECT_EQ(entries.kind(), toml::kind::table);
        EXPECT_EQ(answering(entries), (std::vector<std::string_view>{"table"}));
        EXPECT_EQ(entries.find("port").and_then(&toml::value::as_integer), 8443);
    }

    TEST(toml_value, an_integer_reads_as_floating_only_when_exact) {
        const toml::value small{std::uint32_t{5}};
        EXPECT_EQ(small.kind(), toml::kind::integer);
        EXPECT_EQ(answering(small), (std::vector<std::string_view>{"integer", "floating"}));
        EXPECT_EQ(small.as_integer(), 5);
        EXPECT_EQ(small.as_floating(), 5.0);

        constexpr auto limits = std::numeric_limits<std::int64_t>{};
        EXPECT_EQ(toml::value{std::int64_t{1} << 53}.as_floating(), 0x1p53);
        EXPECT_EQ(toml::value{(std::int64_t{1} << 53) + 1}.as_floating(), std::nullopt);
        EXPECT_EQ(toml::value{limits.max()}.as_floating(), std::nullopt);
        EXPECT_EQ(toml::value{limits.min()}.as_floating(), -0x1p63);

        EXPECT_EQ(toml::value{5.0}.as_integer(), std::nullopt);
    }

    TEST(toml_value, containers_are_edited_in_place) {
        toml::table document{
            {"title", "fileserver"},
            {"http", toml::table{{"port", 8443}}},
            {"files", toml::array{"a.html"}},
        };

        auto files = document.find("files").and_then(&toml::value::edit_array);
        ASSERT_TRUE(files);
        files->get().emplace_back("b.html");

        auto http = document.find("http").and_then(&toml::value::edit_table);
        ASSERT_TRUE(http);
        http->get().set("host", "::1");

        auto title = document.find("title");
        ASSERT_TRUE(title);
        title->get() = "renamed";

        EXPECT_EQ(document, (toml::table{
                                {"title", "renamed"},
                                {"http", toml::table{{"port", 8443}, {"host", "::1"}}},
                                {"files", toml::array{"a.html", "b.html"}},
                            }));

        EXPECT_FALSE(document.find("title").and_then(&toml::value::edit_table));
        EXPECT_FALSE(document.find("http").and_then(&toml::value::edit_array));
    }

    TEST(formats_error, codes_share_one_category) {
        const std::error_code syntax = formats::error_code::invalid_syntax;
        const std::error_code depth = formats::error_code::depth_exceeded;
        const std::error_code unwritable = formats::error_code::invalid_value;

        EXPECT_STREQ(syntax.category().name(), "rei.formats");
        EXPECT_EQ(syntax, std::errc::invalid_argument);
        EXPECT_EQ(unwritable, std::errc::invalid_argument);
        EXPECT_NE(depth, std::errc::invalid_argument);
        EXPECT_EQ(rei::error{depth}.code(), formats::error_code::depth_exceeded);
    }
} // namespace rei::formats::tests
