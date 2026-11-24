#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/formats/json.hpp"

namespace rei::formats::tests {
    namespace {
        static_assert(sizeof(json::value) == 16);
        static_assert(std::constructible_from<json::value, const char*>);
        static_assert(std::constructible_from<json::value, std::uint64_t>);
        static_assert(std::constructible_from<json::value, std::nullptr_t>);
        static_assert(!std::constructible_from<json::value, char>);
        static_assert(std::forward_iterator<json::object::iterator>);

        auto keys(const json::object& members) -> std::vector<std::string> {
            std::vector<std::string> names;
            for (const auto& [key, value] : members) {
                names.emplace_back(key);
            }

            return names;
        }

        auto text(const json::value& value) -> std::string {
            return json::serialize(value).value_or("failed");
        }

        auto parsed(std::string_view source) -> json::value {
            auto document = json::parse(source);
            EXPECT_TRUE(document) << source;
            return std::move(document).value_or(json::value{});
        }
    } // namespace

    TEST(json_value, holds_each_kind_and_answers_only_for_it) {
        const json::value nothing;
        const json::value flag{true};
        const json::value negative{-42};
        const json::value huge{std::numeric_limits<std::uint64_t>::max()};
        const json::value ratio{0.5F};
        const json::value brief{"fourteen bytes"};
        const json::value lengthy{std::string(100, 'x')};

        EXPECT_TRUE(nothing.is_null());
        EXPECT_TRUE(json::value{nullptr}.is_null());
        EXPECT_EQ(nothing.kind(), json::kind::null);
        EXPECT_FALSE(nothing.as_boolean() || nothing.as_integer() || nothing.as_string() ||
                     nothing.as_array());

        EXPECT_EQ(flag.as_boolean(), true);
        EXPECT_FALSE(flag.as_integer());

        EXPECT_EQ(negative.as_integer(), -42);
        EXPECT_FALSE(negative.as_unsigned());
        EXPECT_EQ(negative.as_floating(), -42.0);

        EXPECT_EQ(huge.kind(), json::kind::integer);
        EXPECT_FALSE(huge.as_integer());
        EXPECT_EQ(huge.as_unsigned(), std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(json::value{7U}.as_integer(), 7);

        EXPECT_EQ(ratio.as_floating(), 0.5);
        EXPECT_FALSE(ratio.as_integer());

        EXPECT_EQ(brief.as_string(), "fourteen bytes");
        EXPECT_EQ(lengthy.as_string(), std::string(100, 'x'));
        EXPECT_EQ(json::value{""}.as_string(), "");
        EXPECT_FALSE(brief.as_object());

        EXPECT_NE(json::value{1}, json::value{1.0});
        EXPECT_NE(json::value{-1}, huge);
        EXPECT_EQ(json::value{std::uint8_t{7}}, json::value{7});
    }

    TEST(json_object, keeps_order_and_finds_sets_and_removes) {
        json::object settings{
            {"title", "fileserver"},
            {"http",
             json::object{{"host", "127.0.0.1"}, {"port", 8443}, {"tls", json::object{{"min", "1.3"}}}}},
            {"index_files", json::array{"index.html", "index.htm"}},
            {"title", "renamed"},
        };

        EXPECT_EQ(tests::keys(settings), (std::vector<std::string>{"title", "http", "index_files"}));
        EXPECT_EQ(settings.find("title").and_then(&json::value::as_string), "renamed");
        EXPECT_EQ(settings.find("http", "port").and_then(&json::value::as_integer), 8443);
        EXPECT_EQ(settings.find("http", "tls", "min").and_then(&json::value::as_string), "1.3");
        EXPECT_FALSE(settings.find("http", "missing"));
        EXPECT_FALSE(settings.find("title", "port"));
        EXPECT_TRUE(settings.contains("index_files"));

        settings.set("verbose", true);
        settings.set("a_key_that_does_not_fit_inline", std::string(40, 'v'));
        settings.set("title", 7) = "assigned through the reference";
        EXPECT_EQ(settings.size(), 5U);
        EXPECT_EQ(settings.find("title").and_then(&json::value::as_string), "assigned through the reference");
        EXPECT_EQ(settings.find("a_key_that_does_not_fit_inline").and_then(&json::value::as_string),
                  std::string(40, 'v'));

        EXPECT_TRUE(settings.remove("http"));
        EXPECT_FALSE(settings.remove("http"));
        EXPECT_EQ(tests::keys(settings), (std::vector<std::string>{"title", "index_files", "verbose",
                                                                   "a_key_that_does_not_fit_inline"}));

        for (auto i = 0; i < 100; i++) {
            settings.set(std::to_string(i), i);
        }
        EXPECT_EQ(settings.size(), 104U);
        EXPECT_EQ(settings.find("57").and_then(&json::value::as_integer), 57);

        settings.clear();
        EXPECT_TRUE(settings.empty());
        EXPECT_EQ(tests::text(settings), "{}");
    }

    TEST(json_array, grows_inserts_and_erases) {
        json::array items{1, "two", json::array{3}};
        items.push_back(4.5);
        items.insert(0, nullptr);
        items.push_back(items) = json::object{{"nested", items}};

        EXPECT_EQ(tests::text(items), R"([null,1,"two",[3],4.5,{"nested":[null,1,"two",[3],4.5]}])");
        EXPECT_EQ(items[2].as_string(), "two");

        items.erase(5);
        items.erase(0);
        items[0] = std::string(30, 'a');
        for (auto& item : items) {
            if (item.kind() == json::kind::floating) {
                item = false;
            }
        }

        EXPECT_EQ(tests::text(items), R"(["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","two",[3],false])");

        items.reserve(64);
        items.clear();
        EXPECT_TRUE(items.empty());
    }

    TEST(json_value, equality_ignores_member_order_and_sees_what_lookups_see) {
        const auto ordered = tests::parsed(R"({"a": 1, "b": [true, null, {"c": "d"}]})");
        const auto shuffled = tests::parsed(R"({"b": [true, null, {"c": "d"}], "a": 1})");
        const auto repeated = tests::parsed(R"({"a": 0, "b": [true, null, {"c": "d"}], "a": 1})");
        const json::value built{json::object{
            {"a", 1},
            {"b", json::array{true, nullptr, json::object{{"c", "d"}}}},
        }};

        EXPECT_EQ(ordered, shuffled);
        EXPECT_EQ(ordered, built);
        EXPECT_EQ(ordered, repeated);
        EXPECT_EQ(repeated, ordered);

        EXPECT_NE(ordered, tests::parsed(R"({"a": 1, "b": [null, true, {"c": "d"}]})"));
        EXPECT_NE(ordered, tests::parsed(R"({"a": 1, "b": [true, null, {"c": "d"}], "e": 2})"));
        EXPECT_NE(ordered, tests::parsed(R"({"a": 1.0, "b": [true, null, {"c": "d"}]})"));

        const auto& members = repeated.as_object()->get();
        EXPECT_EQ(members.size(), 3U);
        EXPECT_EQ(members.find("a").and_then(&json::value::as_integer), 1);
    }

    TEST(json_value, copies_are_deep_and_a_parsed_document_is_edited_in_place) {
        auto document = tests::parsed(R"({
            "name": "a string that is long enough to live in the document's buffer",
            "tags": ["one", "two"],
            "limits": {"body": 1048576, "depth": 128}
        })");
        const auto original = document;

        auto& root = document.edit_object()->get();
        root.find("tags")->get().edit_array()->get().push_back("three");
        root.find("limits")->get().edit_object()->get().set("depth", 64);
        root.find("limits")->get().edit_object()->get().remove("body");
        root.set("name", "short");
        root.set("added", json::array{1, 2, 3});

        EXPECT_EQ(tests::text(document),
                  R"({"name":"short","tags":["one","two","three"],"limits":{"depth":64},"added":[1,2,3]})");
        EXPECT_EQ(original.find("limits", "depth").and_then(&json::value::as_integer), 128);
        EXPECT_EQ(original.find("tags").and_then(&json::value::as_array)->size(), 2U);

        auto tags = std::move(root.find("tags")->get());
        document = json::value{};
        EXPECT_EQ(tests::text(tags), R"(["one","two","three"])");

        auto reparsed = tests::parsed(R"({"outer": {"inner": [1, 2, {"deep": "value"}]}})");
        json::array holder;
        holder.push_back(std::move(reparsed));
        holder.push_back(holder[0].find("outer", "inner")->get());
        holder[0] = holder[0].find("outer", "inner")->get();
        EXPECT_EQ(tests::text(holder), R"([[1,2,{"deep":"value"}],[1,2,{"deep":"value"}]])");

        auto self = tests::parsed(R"({"keep": {"me": [1, 2, 3]}, "drop": "this"})");
        self = std::move(self.edit_object()->get().find("keep")->get());
        EXPECT_EQ(tests::text(self), R"({"me":[1,2,3]})");
    }
} // namespace rei::formats::tests
