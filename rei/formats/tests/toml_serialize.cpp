#include <chrono>
#include <cstdint>
#include <limits>
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

        auto written(const toml::value& value) -> std::string {
            const auto text = toml::serialize(value);
            return text ? *text : "refused: " + text.error().detail();
        }

        template <typename Document> auto refusal(const Document& document) -> std::string {
            const auto text = toml::serialize(document);
            if (text || text.error().code() != formats::error_code::invalid_value) {
                return "not refused";
            }

            return text.error().detail();
        }
    } // namespace

    TEST(toml_serialize, writes_one_layout_that_reads_back_equal) {
        const toml::table servers{
            {"alpha", toml::table{{"ip", "10.0.0.1"}, {"role", "frontend"}}},
            {"beta", toml::table{{"ip", "10.0.0.2"}}},
        };
        const toml::array mirrors{
            toml::table{{"host", "a.example"}, {"tls", toml::table{{"min_version", "1.3"}}}},
            toml::table{{"host", "b.example"}},
        };
        const toml::date_time started{
            .date = 1979y / 5 / 27,
            .time = {.hour = 7, .minute = 32, .nanosecond = 500'000'000},
            .offset = -7h,
        };
        const toml::table http{
            {"limits", toml::table{}},
            {"host", "127.0.0.1"},
            {"port", 8443},
            {"index_files", toml::array{"index.html", "index.htm"}},
            {"routes", toml::array{toml::table{{"path", "/"}, {"cache", true}}, "fallback"}},
            {"started", started},
        };
        const toml::table document{
            {"servers", servers},     {"title", "fileserver"},
            {"mirrors", mirrors},     {"http", http},
            {"ratio", 0.5},           {"dotted.key", toml::table{{"needs quotes", 1}, {"", 2}}},
            {"empty", toml::array{}},
        };

        const auto text = toml::serialize(document);
        ASSERT_TRUE(text) << text.error();
        EXPECT_EQ(*text, R"(title = "fileserver"
ratio = 0.5
empty = []

[servers.alpha]
ip = "10.0.0.1"
role = "frontend"

[servers.beta]
ip = "10.0.0.2"

[[mirrors]]
host = "a.example"

[mirrors.tls]
min_version = "1.3"

[[mirrors]]
host = "b.example"

[http]
host = "127.0.0.1"
port = 8443
index_files = ["index.html", "index.htm"]
routes = [{ path = "/", cache = true }, "fallback"]
started = 1979-05-27T07:32:00.5-07:00

[http.limits]

["dotted.key"]
"needs quotes" = 1
"" = 2
)");

        const auto read_back = toml::parse(*text);
        ASSERT_TRUE(read_back) << read_back.error();
        EXPECT_EQ(*read_back, document);

        EXPECT_EQ(toml::serialize(toml::table{}), "");
    }

    TEST(toml_serialize, writes_a_single_value_as_it_stands_after_the_equals_sign) {
        constexpr auto infinity = std::numeric_limits<double>::infinity();
        constexpr std::string_view escapes =
            "quote \" slash \\ tab \t line \n bell \x07 delete \x7F \xC3\xA9";
        constexpr std::string_view escaped =
            "\"quote \\\" slash \\\\ tab \\t line \\n bell \\u0007 delete \\u007F \xC3\xA9\"";

        const toml::time precise{.hour = 23, .minute = 59, .second = 59, .nanosecond = 123'456'789};
        const toml::date_time local{.date = 1979y / 5 / 27, .time = {.hour = 7, .minute = 32}};
        const toml::date_time utc{.date = local.date, .time = local.time, .offset = 0min};
        const toml::date_time india{.date = local.date, .time = local.time, .offset = 5h + 30min};
        const toml::table inner{{"d", toml::array{toml::table{{"e", true}}}}};
        const toml::table nested{{"a", 1}, {"b c", inner}};

        for (const auto& [value, text] : {
                 std::pair<toml::value, std::string_view>{"text", R"("text")"},
                 {escapes, escaped},
                 {5, "5"},
                 {std::numeric_limits<std::int64_t>::min(), "-9223372036854775808"},
                 {1.0, "1.0"},
                 {-0.0, "-0.0"},
                 {0.1, "0.1"},
                 {1e21, "1e+21"},
                 {5e-324, "5e-324"},
                 {infinity, "inf"},
                 {-infinity, "-inf"},
                 {std::numeric_limits<double>::quiet_NaN(), "nan"},
                 {true, "true"},
                 {toml::date{1y / 1 / 1}, "0001-01-01"},
                 {toml::time{.hour = 7, .minute = 32}, "07:32:00"},
                 {precise, "23:59:59.123456789"},
                 {local, "1979-05-27T07:32:00"},
                 {utc, "1979-05-27T07:32:00Z"},
                 {india, "1979-05-27T07:32:00+05:30"},
                 {toml::array{}, "[]"},
                 {toml::array{1, "two", toml::array{3.0}}, R"([1, "two", [3.0]])"},
                 {toml::table{}, "{}"},
                 {nested, R"({ a = 1, "b c" = { d = [{ e = true }] } })"},
             }) {
            EXPECT_EQ(tests::written(value), text);
        }
    }

    TEST(toml_serialize, refuses_what_toml_cannot_hold) {
        EXPECT_EQ(tests::refusal(toml::value{"\xFF"}), "the string is not valid UTF-8");
        EXPECT_EQ(tests::refusal(toml::table{{"http", toml::table{{"names", toml::array{"ok", "\xC3"}}}}}),
                  "http.names[1]: the string is not valid UTF-8");
        EXPECT_EQ(tests::refusal(toml::table{{"mirrors", toml::array{toml::table{{"host", "\x80"}}}}}),
                  "mirrors[0].host: the string is not valid UTF-8");
        EXPECT_EQ(tests::refusal(toml::table{{"http", toml::table{{"\xFF", 1}}}}),
                  "http: a key is not valid UTF-8");
        EXPECT_EQ(tests::refusal(toml::table{{"\xFF", toml::table{}}}), "a key is not valid UTF-8");

        EXPECT_EQ(tests::refusal(toml::table{{"day", toml::date{2024y / 2 / 30}}}),
                  "day: 2024-2-30 is not a date");
        EXPECT_EQ(tests::refusal(toml::value{toml::date{10000y / 1 / 1}}), "10000-1-1 is not a date");
        EXPECT_EQ(tests::refusal(toml::value{toml::time{.hour = 24}}), "24:0:0.0 is not a time");
        EXPECT_EQ(tests::refusal(toml::value{toml::time{.nanosecond = 1'000'000'000}}),
                  "0:0:0.1000000000 is not a time");
        EXPECT_EQ(tests::refusal(toml::value{toml::date_time{.date = 2024y / 2 / 29, .offset = 24h}}),
                  "an offset of 1440 minutes is out of range");
    }
} // namespace rei::formats::tests
