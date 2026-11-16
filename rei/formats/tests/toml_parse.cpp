#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/toml.hpp"

namespace rei::formats::tests {
    namespace {
        using namespace std::chrono_literals;

        auto failure(std::string_view text) -> std::string {
            const auto document = toml::parse(text);
            if (document) {
                return "parsed";
            }
            if (document.error().code() != formats::error_code::invalid_syntax) {
                return document.error().code().message();
            }

            return document.error().detail();
        }

        auto line_of(const toml::value& value) -> std::optional<std::uint32_t> {
            return value.line();
        }

        auto nested(std::size_t depth) -> std::string {
            return "a = " + std::string(depth, '[') + std::string(depth, ']');
        }

        auto dotted(std::size_t parts) -> std::string {
            std::string key{"a"};
            for (auto i = 1uz; i < parts; i++) {
                key += ".a";
            }

            return key;
        }
    } // namespace

    TEST(toml_parse, builds_the_document_and_remembers_lines) {
        const auto document = toml::parse(R"(title = "fileserver"

[http]
host = "127.0.0.1"
port = 8443
ratio = 0.5
verbose = true
index_files = [
    "index.html",
    "index.htm",
]
started = 1979-05-27T07:32:00-07:00
tls = { min_version = "1.3" }
limits.body = 1_048_576

[[mirrors]]
host = "a.example"

[[mirrors]]
host = "b.example"
)");
        ASSERT_TRUE(document) << document.error();

        const toml::date_time started{
            .date = 1979y / 5 / 27,
            .time = {.hour = 7, .minute = 32},
            .offset = -7h,
        };
        EXPECT_EQ(*document, (toml::table{
                                 {"title", "fileserver"},
                                 {"http",
                                  toml::table{
                                      {"host", "127.0.0.1"},
                                      {"port", 8443},
                                      {"ratio", 0.5},
                                      {"verbose", true},
                                      {"index_files", toml::array{"index.html", "index.htm"}},
                                      {"started", started},
                                      {"tls", toml::table{{"min_version", "1.3"}}},
                                      {"limits", toml::table{{"body", 1'048'576}}},
                                  }},
                                 {"mirrors",
                                  toml::array{
                                      toml::table{{"host", "a.example"}},
                                      toml::table{{"host", "b.example"}},
                                  }},
                             }));

        const auto line = [&](const auto&... keys) -> std::optional<std::uint32_t> {
            return document->find(keys...).and_then(tests::line_of);
        };

        EXPECT_EQ(line("title"), 1u);
        EXPECT_EQ(line("http"), 3u);
        EXPECT_EQ(line("http", "port"), 5u);
        EXPECT_EQ(line("http", "index_files"), 8u);
        EXPECT_EQ(line("http", "tls"), 13u);
        EXPECT_EQ(line("http", "tls", "min_version"), 13u);
        EXPECT_EQ(line("http", "limits"), 14u);
        EXPECT_EQ(line("http", "limits", "body"), 14u);
        EXPECT_EQ(line("mirrors"), 16u);

        const auto files = document->find("http", "index_files").and_then(&toml::value::as_array);
        ASSERT_TRUE(files && files->size() == 2);
        EXPECT_EQ((*files)[0].line(), 9u);
        EXPECT_EQ((*files)[1].line(), 10u);

        const auto mirrors = document->find("mirrors").and_then(&toml::value::as_array);
        ASSERT_TRUE(mirrors && mirrors->size() == 2);
        EXPECT_EQ((*mirrors)[0].line(), 16u);
        EXPECT_EQ((*mirrors)[1].line(), 19u);
        EXPECT_EQ((*mirrors)[1].find("host").and_then(tests::line_of), 20u);
    }

    TEST(toml_parse, tells_a_float_with_a_long_exponent_from_a_date) {
        const auto document = toml::parse("small = 1.5e-00003\nscaled = 250e-00002\nday = 1979-05-27\n");
        ASSERT_TRUE(document) << document.error();

        EXPECT_EQ(document->find("small").and_then(&toml::value::as_floating), 1.5e-3);
        EXPECT_EQ(document->find("scaled").and_then(&toml::value::as_floating), 2.5);
        EXPECT_EQ(document->find("day").and_then(&toml::value::as_date), 1979y / 5 / 27);
    }

    TEST(toml_parse, errors_name_the_place_and_the_reason) {
        for (const auto& [text, detail] : {
                 std::pair<std::string_view, std::string_view>{"key = ", "1:7: expected a value"},
                 {"a = 1\nb 2", "2:3: expected '=' after a key"},
                 {"a = 1 b = 2", "1:7: expected a newline after a value"},
                 {"[http] port = 1", "1:8: expected a newline after a table header"},
                 {"[http\nport = 1", "1:6: expected ']' after a table name"},
                 {"\xC3\xA9 = 1", "1:1: expected a key"},
                 {"k = \"h\xC3\xA9\xC3\xA9\" x", "1:11: expected a newline after a value"},
                 {"a = \"\xFF\"", "1:6: invalid UTF-8"},
                 {"a = 1 # \x01", "1:9: control character U+0001 in a comment"},
                 {"a = \"one\ttwo\x7F\"", "1:13: control character U+007F in a string"},
                 {"a = \"open", "1:5: unterminated string"},
                 {"a = \"\"\"open\n\n", "1:5: unterminated string"},
                 {R"(a = "x\q")", R"(1:7: '\q' is not an escape)"},
                 {R"(a = "\uD800")", R"(1:6: '\uD800' is not a Unicode scalar value)"},
                 {R"(a = "\u12")", R"(1:6: '\u' must be followed by 4 hexadecimal digits)"},
                 {"a = 01", "1:5: '01' is not a valid value"},
                 {"a = 1__0", "1:5: '1__0' is not a valid value"},
                 {"a = +0x10", "1:5: '+0x10' is not a valid value"},
                 {"a = yes", "1:5: 'yes' is not a valid value"},
                 {"a = 9223372036854775808", "1:5: 9223372036854775808 is out of range for an integer"},
                 {"a = 1e999", "1:5: 1e999 is out of range for a float"},
                 {"a = 2024-02-30", "1:5: '2024-02-30' is not a valid date"},
                 {"a = 2024-02-29T24:00:00", "1:5: '2024-02-29T24:00:00' is not a valid date-time"},
                 {"a = 2024-02-29_07:32:00", "1:5: '2024-02-29_07:32:00' is not a valid date-time"},
                 {"a = 07:60", "1:5: '07:60' is not a valid time"},
                 {"a = 07:32:60", "1:5: '07:32:60' is not a valid time"},
                 {"a = [1, 2", "1:10: expected ',' or ']' in an array"},
                 {"a = {b = 1 c = 2}", "1:12: expected ',' or '}' in an inline table"},
                 {"[http]\nport = 1\nport = 2", "3:1: key 'http.port' is defined twice"},
                 {"a = {b = 1, b = 2}", "1:13: key 'b' is defined twice"},
                 {"[http]\n[http]", "2:1: table 'http' is defined twice"},
                 {"[a]\nb.c = 1\n[a.b]", "3:1: table 'a.b' is defined twice"},
                 {
                     "[a.b]\n[a]\nb.c = 1",
                     "3:1: table 'a.b' is already defined and can't be extended with a dotted key",
                 },
                 {"a = 1\n[a.b]", "2:1: 'a' is not a table"},
                 {"a = {b = 1}\na.c = 2", "2:1: 'a' was defined inline and can't be extended"},
                 {"a = {b = 1}\n[a.c]", "2:1: 'a.c' was defined inline and can't be extended"},
                 {"mirrors = []\n[[mirrors]]", "2:1: 'mirrors' was defined inline and can't be extended"},
                 {"[mirrors]\n[[mirrors]]", "2:1: 'mirrors' is not an array of tables"},
             }) {
            EXPECT_EQ(tests::failure(text), detail) << text;
        }
    }

    TEST(toml_parse, limits_nesting_to_128_levels) {
        EXPECT_TRUE(toml::parse(tests::nested(128)));
        EXPECT_TRUE(toml::parse(tests::dotted(129) + " = 1"));
        EXPECT_TRUE(toml::parse("[" + tests::dotted(128) + "]"));
        EXPECT_TRUE(toml::parse("[[" + tests::dotted(127) + "]]"));
        EXPECT_TRUE(toml::parse(tests::dotted(127) + " = [{a = 1}]"));

        const auto too_deep = [](const std::string& text) -> std::string {
            const auto document = toml::parse(text);
            if (document || document.error().code() != formats::error_code::depth_exceeded) {
                return "not refused";
            }

            return document.error().detail();
        };

        EXPECT_EQ(too_deep(tests::nested(129)), "1:133: the document nests deeper than 128 levels");
        EXPECT_EQ(too_deep(tests::dotted(130) + " = 1"), "1:1: the document nests deeper than 128 levels");
        EXPECT_EQ(too_deep("[" + tests::dotted(129) + "]"), "1:1: the document nests deeper than 128 levels");
        EXPECT_EQ(too_deep("[[" + tests::dotted(128) + "]]"),
                  "1:1: the document nests deeper than 128 levels");
        EXPECT_EQ(too_deep(tests::dotted(128) + " = [{a = 1}]"),
                  "1:" + std::to_string(tests::dotted(128).size() + 5) +
                      ": the document nests deeper than 128 levels");
    }

    TEST(toml_parse, settles_what_the_format_leaves_open) {
        const auto document = toml::parse("\xEF\xBB\xBF"
                                          "text = \"\"\"\r\nfirst\r\nsecond \\\r\n   third\"\"\"\r\n"
                                          "raw = '''\r\none\r\ntwo'''\r\n"
                                          "moment = 1979-05-27 07:32Z\r\n"
                                          "precise = 07:32:00.1234567899\r\n"
                                          "whole = 1e3\r\n");
        ASSERT_TRUE(document) << document.error();

        EXPECT_EQ(document->find("text").and_then(&toml::value::as_string), "first\nsecond third");
        EXPECT_EQ(document->find("raw").and_then(&toml::value::as_string), "one\ntwo");
        EXPECT_EQ(
            document->find("moment").and_then(&toml::value::as_date_time),
            (toml::date_time{.date = 1979y / 5 / 27, .time = {.hour = 7, .minute = 32}, .offset = 0min}));
        EXPECT_EQ(document->find("precise").and_then(&toml::value::as_time),
                  (toml::time{.hour = 7, .minute = 32, .nanosecond = 123'456'789}));
        EXPECT_EQ(document->find("whole").transform(&toml::value::kind), toml::kind::floating);
        EXPECT_EQ(document->find("moment").and_then(tests::line_of), 8u);

        EXPECT_EQ(tests::failure("a = 1\xEF\xBB\xBF"), "1:6: expected a newline after a value");
    }
} // namespace rei::formats::tests
