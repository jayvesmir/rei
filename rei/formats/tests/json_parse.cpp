#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/json.hpp"

namespace rei::formats::tests {
    namespace {
        auto failure(std::string_view text, const json::parse_options& options = {}) -> std::string {
            const auto document = json::parse(text, options);
            if (document) {
                return "parsed";
            }
            if (document.error().code() != formats::error_code::invalid_syntax) {
                return document.error().code().message();
            }

            return document.error().detail();
        }

        auto nested(std::size_t depth) -> std::string {
            return std::string(depth, '[') + std::string(depth, ']');
        }

        auto listing(std::size_t members, std::size_t indent, std::size_t stem) -> std::string {
            std::string text = "[";

            for (auto row = 0uz; row < 3; row++) {
                text += std::format("{}\n{}{{", row == 0 ? "" : ",", std::string(indent, ' '));
                for (auto i = 0uz; i < members; i++) {
                    text +=
                        std::format("{}\n{}\"{}\": \"{}\"", i == 0 ? "" : ",", std::string(indent * 2, ' '),
                                    std::string(stem + i, 'k'), std::string(stem + row + i, 'v'));
                }
                text += std::format("\n{}}}", std::string(indent, ' '));
            }

            return text + "\n]";
        }
    } // namespace

    TEST(json_parse, reads_every_kind_of_value) {
        const auto document = json::parse("\xEF\xBB\xBF"
                                          R"(
            {
                "null": null, "yes": true, "no": false,
                "small": -17, "lowest": -9223372036854775808, "highest": 18446744073709551615,
                "beyond": 18446744073709551616, "below": -9223372036854775809, "zero": -0,
                "ratio": 0.1, "scaled": -1.5E+3, "tiny": 1e-400, "precise": 0.30000000000000004,
                "long": 3.141592653589793238462643383279, "nineteen": 1234567890123456789,
                "escapes": "\"\\\/\b\f\n\r\té😀",
                "raw": "é😀",
                "thirty two bytes exactly, no less": "and a value that runs well past the first sixteen bytes",
                "empty": "", "list": [[], {}, [1, [2, [3]]]]
            })");
        ASSERT_TRUE(document) << document.error();

        const auto integer = [&](std::string_view key) -> std::optional<std::int64_t> {
            return document->find(key).and_then(&json::value::as_integer);
        };
        const auto floating = [&](std::string_view key) -> std::optional<double> {
            return document->find(key).and_then(&json::value::as_floating);
        };
        const auto kind = [&](std::string_view key) -> std::optional<json::kind> {
            return document->find(key).transform(&json::value::kind);
        };

        EXPECT_TRUE(document->find("null")->get().is_null());
        EXPECT_EQ(document->find("yes").and_then(&json::value::as_boolean), true);
        EXPECT_EQ(document->find("no").and_then(&json::value::as_boolean), false);

        EXPECT_EQ(integer("small"), -17);
        EXPECT_EQ(integer("lowest"), std::numeric_limits<std::int64_t>::min());
        EXPECT_EQ(document->find("highest").and_then(&json::value::as_unsigned),
                  std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(integer("highest"), std::nullopt);
        EXPECT_EQ(integer("nineteen"), 1234567890123456789);
        EXPECT_EQ(integer("zero"), 0);

        EXPECT_EQ(kind("beyond"), json::kind::floating);
        EXPECT_EQ(floating("beyond"), 18446744073709551616.0);
        EXPECT_EQ(floating("below"), -9223372036854775809.0);
        EXPECT_EQ(floating("ratio"), 0.1);
        EXPECT_EQ(floating("scaled"), -1500.0);
        EXPECT_EQ(floating("tiny"), 0.0);
        EXPECT_EQ(floating("precise"), 0.30000000000000004);
        EXPECT_EQ(floating("long"), 3.141592653589793);

        EXPECT_EQ(document->find("escapes").and_then(&json::value::as_string),
                  "\"\\/\b\f\n\r\t\xC3\xA9\xF0\x9F\x98\x80");
        EXPECT_EQ(document->find("raw").and_then(&json::value::as_string), "\xC3\xA9\xF0\x9F\x98\x80");
        EXPECT_EQ(document->find("thirty two bytes exactly, no less").and_then(&json::value::as_string),
                  "and a value that runs well past the first sixteen bytes");
        EXPECT_EQ(document->find("empty").and_then(&json::value::as_string), "");

        const auto list = document->find("list").and_then(&json::value::as_array);
        ASSERT_TRUE(list && list->size() == 3);
        EXPECT_TRUE((*list)[0].as_array()->empty());
        EXPECT_TRUE((*list)[1].as_object()->get().empty());
        EXPECT_EQ((*list)[2], (json::value{json::array{1, json::array{2, json::array{3}}}}));

        EXPECT_EQ(json::parse(" 42 ").and_then(json::serialize), "42");
        EXPECT_EQ(json::parse(R"("alone")").and_then(json::serialize), R"("alone")");
        EXPECT_EQ(json::parse("\t[\r\n]\n").and_then(json::serialize), "[]");
    }

    TEST(json_parse, errors_name_the_place_and_the_reason) {
        for (const auto& [text, detail] : {
                 std::pair<std::string_view, std::string_view>{"", "1:1: expected a value"},
                 {"  \n ", "2:2: expected a value"},
                 {"nul", "1:1: expected a value"},
                 {"[1, 2", "1:6: expected ',' or ']' after an array element"},
                 {"[1 2]", "1:4: expected ',' or ']' after an array element"},
                 {"[1,]", "1:4: expected a value"},
                 {"{\"a\": 1,}", "1:9: expected a member name"},
                 {"{\n  \"a\" 1\n}", "2:7: expected ':' after a member name"},
                 {"{\"a\": 1 \"b\": 2}", "1:9: expected ',' or '}' after an object member"},
                 {"{a: 1}", "1:2: expected a member name"},
                 {"[1] 2", "1:5: unexpected text after the document"},
                 {"\"open", "1:6: the string is not closed"},
                 {"\"a\tb\"", "1:3: control character U+0009 in a string"},
                 {R"("\q")", R"(1:2: '\q' is not an escape)"},
                 {R"("\u12G4")", R"(1:2: '\u' must be followed by four hexadecimal digits)"},
                 {R"("\uD83D")", R"(1:2: the surrogate '\uD83D' has no second half)"},
                 {R"(["é", "\uDE00"])", R"(1:8: the surrogate '\uDE00' has no first half)"},
                 {"\"\xC3\x28\"", "1:2: the string is not valid UTF-8"},
                 {"\"\xED\xA0\x80\"", "1:2: the string is not valid UTF-8"},
                 {"[01]", "1:2: a number can't start with a zero"},
                 {"[-]", "1:3: expected a digit after '-'"},
                 {"[1.]", "1:4: expected a digit after the decimal point"},
                 {"[1e+]", "1:5: expected a digit in the exponent"},
                 {"[.5]", "1:2: expected a value"},
                 {"[1e400]", "1:2: 1e400 is out of range for a number"},
                 {"[\x0B]", "1:2: expected a value"},
                 {"[NaN]", "1:2: expected a value"},
             }) {
            EXPECT_EQ(tests::failure(text), detail) << text;
        }
    }

    TEST(json_parse, limits_how_deep_a_document_nests) {
        EXPECT_TRUE(json::parse(tests::nested(128)));
        EXPECT_TRUE(json::parse(tests::nested(3), {.max_depth = 3}));
        EXPECT_EQ(json::parse(tests::nested(5000), {.max_depth = 5000}).and_then(json::serialize),
                  tests::nested(5000));

        const auto deep = json::parse(tests::nested(129));
        ASSERT_FALSE(deep);
        EXPECT_EQ(deep.error().code(), formats::error_code::depth_exceeded);
        EXPECT_EQ(deep.error().detail(), "1:129: the document nests deeper than 128 levels");

        EXPECT_EQ(tests::failure(R"({"a": [{"b": 1}]})", {.max_depth = 2}), "document nests too deeply");
        EXPECT_EQ(tests::failure("1", {.max_depth = 0}), "parsed");
    }

    TEST(json_parse, never_reads_past_the_end_of_the_text) {
        for (const auto* last : {"-1234567890123456789.123456789", "1.2345678901234567e+105", "12345678",
                                 "-0.5", "\"a string of thirty-one bytes....\"", "true"}) {
            for (auto indent : {0uz, 7uz, 15uz, 16uz, 31uz, 40uz}) {
                for (auto filler = 0uz; filler < 140; filler++) {
                    std::string text = "[";
                    for (auto i = 0uz; i < filler; i++) {
                        text += "0,";
                    }
                    text += std::format("\n{}{}]", std::string(indent, ' '), last);

                    const std::vector<char> exact{text.begin(), text.end()};
                    const auto document = json::parse(std::string_view{exact.data(), exact.size()});
                    ASSERT_TRUE(document) << text << "\n" << document.error();
                    EXPECT_EQ(document->as_array()->size(), filler + 1) << text;

                    const std::vector<char> cut{text.begin(), text.end() - 1};
                    EXPECT_FALSE(json::parse(std::string_view{cut.data(), cut.size()})) << text;
                }
            }
        }
    }

    TEST(json_parse, reads_strings_and_layouts_of_every_length) {
        for (auto length = 0uz; length < 150; length++) {
            for (const auto* filler : {"x", "\xC3\xA9", "\\n"}) {
                std::string body;
                std::string expected;
                for (auto i = 0uz; i < length; i++) {
                    body += filler;
                    expected += std::string_view{filler} == "\\n" ? "\n" : filler;
                }

                const auto document = json::parse(std::format(R"(["{0}", {{"{0}": "{0}"}}])", body));
                ASSERT_TRUE(document) << length << " " << document.error();

                const auto items = document->as_array();
                ASSERT_TRUE(items && items->size() == 2);
                EXPECT_EQ((*items)[0].as_string(), expected) << length;
                EXPECT_EQ((*items)[1].find(expected).and_then(&json::value::as_string), expected) << length;
            }

            const auto digits = std::string(length % 25, '7');
            const auto padded = json::parse(std::format("{}[1{}]", std::string(length, ' '), digits));
            ASSERT_TRUE(padded) << length << " " << padded.error();
            EXPECT_EQ(std::format("1{}", digits).size() > 19 ? "skipped" : *json::serialize(*padded),
                      std::format("1{}", digits).size() > 19 ? "skipped" : std::format("[1{}]", digits));
        }

        for (auto members = 1uz; members < 8; members++) {
            for (auto indent = 0uz; indent < 20; indent++) {
                for (auto stem : {1uz, 9uz, 14uz, 15uz, 16uz, 30uz, 31uz, 40uz}) {
                    const auto text = tests::listing(members, indent, stem);
                    const auto document = json::parse(text);
                    ASSERT_TRUE(document) << text << "\n" << document.error();

                    const auto rows = document->as_array();
                    ASSERT_TRUE(rows && rows->size() == 3) << text;
                    for (auto row = 0uz; row < 3; row++) {
                        const auto& fields = (*rows)[row].as_object()->get();
                        ASSERT_EQ(fields.size(), members) << text;
                        for (auto i = 0uz; i < members; i++) {
                            EXPECT_EQ(
                                fields.find(std::string(stem + i, 'k')).and_then(&json::value::as_string),
                                std::string(stem + row + i, 'v'))
                                << text;
                        }
                    }
                }
            }
        }
    }
} // namespace rei::formats::tests
