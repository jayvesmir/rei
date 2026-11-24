#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/formats/error_code.hpp"
#include "rei/formats/json.hpp"

namespace rei::formats::tests {
    namespace {
        auto refusal(const json::value& value) -> std::string {
            const auto text = json::serialize(value);
            if (text) {
                return "written";
            }
            if (text.error().code() != formats::error_code::invalid_value) {
                return text.error().code().message();
            }

            return text.error().detail();
        }
    } // namespace

    TEST(json_serialize, writes_compact_json_in_member_order) {
        const json::value document{json::object{
            {"title", "file\"server\\ \x01\n\t caf\xC3\xA9"},
            {"numbers", json::array{0, -1, 1234567890123, std::numeric_limits<std::int64_t>::min(),
                                    std::numeric_limits<std::uint64_t>::max(), 1.0, -0.0, 0.1, 1e15, 1e16,
                                    1e21, 1.5e-7, 5e-324}},
            {"flags", json::array{true, false, nullptr}},
            {"empty", json::array{json::array{}, json::object{}, ""}},
            {"a key longer than the fourteen bytes kept inline", std::string(70, 'z')},
        }};

        const auto text = json::serialize(document);
        ASSERT_TRUE(text) << text.error();
        EXPECT_EQ(*text,
                  R"({"title":"file\"server\\ \u0001\n\t café",)"
                  R"("numbers":[0,-1,1234567890123,-9223372036854775808,18446744073709551615,1.0,-0.0,0.1,)"
                  R"(1000000000000000.0,1e+16,1e+21,1.5e-07,5e-324],)"
                  R"("flags":[true,false,null],"empty":[[],{},""],)"
                  R"("a key longer than the fourteen bytes kept inline":")" +
                      std::string(70, 'z') + R"("})");

        const auto read_back = json::parse(*text);
        ASSERT_TRUE(read_back) << read_back.error();
        EXPECT_EQ(*read_back, document);
        EXPECT_EQ(json::serialize(*read_back), *text);

        json::array wide;
        for (auto i = 0; i < 20000; i++) {
            wide.push_back(
                json::object{{"id", i}, {"name", std::string(static_cast<std::size_t>(i % 90), 'n')}});
        }
        const auto large = json::serialize(wide);
        ASSERT_TRUE(large) << large.error();
        EXPECT_EQ(json::parse(*large), json::value{wide});
    }

    TEST(json_serialize, refuses_what_json_cannot_hold) {
        EXPECT_EQ(tests::refusal(std::numeric_limits<double>::quiet_NaN()), "nan can't be written as JSON");
        EXPECT_EQ(
            tests::refusal(json::object{
                {"http", json::object{{"ratio", json::array{1, -std::numeric_limits<double>::infinity()}}}}}),
            "http.ratio[1]: an infinite number can't be written as JSON");
        EXPECT_EQ(tests::refusal(json::array{"fine", json::object{{"name", "caf\xE9"}}}),
                  "[1].name: the string is not valid UTF-8");
        EXPECT_EQ(tests::refusal(json::object{{"http", json::object{{"bad\xFFkey", 1}}}}),
                  "http: a key is not valid UTF-8");
        EXPECT_EQ(tests::refusal(std::string(40, 'a') + "\xC3"), "the string is not valid UTF-8");
        EXPECT_EQ(tests::refusal("\xC3\xA9 \xF0\x9F\x98\x80"), "written");
    }
} // namespace rei::formats::tests
