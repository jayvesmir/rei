#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    namespace h1 = http::h1;

    TEST(http_head, parses_pipelined_requests_into_a_reused_head) {
        constexpr std::string_view first = "GET /search?q=rei HTTP/1.1\r\n"
                                           "Host: example.com\r\n"
                                           "Accept:\t text/html, */* \t\r\n"
                                           "X-Empty:\r\n"
                                           "X-Name: J\xF6rg\r\n"
                                           "\r\n";
        constexpr std::string_view second = "OPTIONS * HTTP/1.0\r\n\r\n";
        const auto input = std::string{first} + std::string{second};
        http::request_head head;

        auto size = h1::parse_head(tests::as_bytes(input), head);
        ASSERT_TRUE(size) << size.error();
        EXPECT_EQ(*size, first.size());
        EXPECT_EQ(head.method, "GET");
        EXPECT_EQ(head.target, "/search?q=rei");
        EXPECT_EQ(head.target.data(), input.data() + 4);
        EXPECT_EQ(head.version, http::version::http_1_1);

        const std::vector<http::header> expected{
            {.name = "Host", .value = "example.com"},
            {.name = "Accept", .value = "text/html, */*"},
            {.name = "X-Empty", .value = ""},
            {.name = "X-Name", .value = "J\xF6rg"},
        };
        EXPECT_EQ(head.headers, expected);
        EXPECT_EQ(http::find_header(head.headers, "accept"), "text/html, */*");
        EXPECT_EQ(http::find_header(head.headers, "Cookie"), std::nullopt);

        size = h1::parse_head(tests::as_bytes(input).subspan(first.size()), head);
        ASSERT_TRUE(size) << size.error();
        EXPECT_EQ(*size, second.size());
        EXPECT_EQ(head.method, "OPTIONS");
        EXPECT_EQ(head.target, "*");
        EXPECT_EQ(head.version, http::version::http_1_0);
        EXPECT_TRUE(head.headers.empty());
    }

    TEST(http_head, parses_status_lines) {
        struct status_case {
            std::string_view line;
            http::version version;
            http::status status;
            std::string_view reason;
        };

        for (const auto& [line, version, status, reason] : {
                 status_case{
                     .line = "HTTP/1.0 404 Not Found",
                     .version = http::version::http_1_0,
                     .status = http::status::not_found,
                     .reason = "Not Found",
                 },
                 status_case{
                     .line = "HTTP/1.1 204 ",
                     .version = http::version::http_1_1,
                     .status = http::status::no_content,
                     .reason = "",
                 },
                 status_case{
                     .line = "HTTP/1.1 204",
                     .version = http::version::http_1_1,
                     .status = http::status::no_content,
                     .reason = "",
                 },
                 status_case{
                     .line = "HTTP/1.1 299 Caf\xE9 \t closed",
                     .version = http::version::http_1_1,
                     // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                     .status = http::status{299},
                     .reason = "Caf\xE9 \t closed",
                 },
             }) {
            const auto input = std::string{line} + "\r\nServer: rei\r\n\r\n";
            http::response_head head;

            const auto size = h1::parse_head(tests::as_bytes(input), head);
            ASSERT_TRUE(size) << line << ": " << size.error();
            EXPECT_EQ(*size, input.size()) << line;
            EXPECT_EQ(head.version, version) << line;
            EXPECT_EQ(head.status, status) << line;
            EXPECT_EQ(head.reason, reason) << line;
            EXPECT_EQ(http::find_header(head.headers, "server"), "rei") << line;
        }
    }

    TEST(http_head, is_incomplete_until_the_last_byte_arrives) {
        constexpr std::string_view input = "POST /upload HTTP/1.1\r\n"
                                           "Host: example.com\r\n"
                                           "Content-Length: 3\r\n"
                                           "\r\n";
        http::request_head head;

        for (auto size = 0uz; size < input.size(); size++) {
            const auto parsed = h1::parse_head(tests::as_bytes(input.substr(0, size)), head);
            ASSERT_TRUE(parsed) << size << ": " << parsed.error();
            EXPECT_EQ(*parsed, std::nullopt) << size;
        }

        const auto parsed = h1::parse_head(tests::as_bytes(input), head);
        ASSERT_TRUE(parsed) << parsed.error();
        EXPECT_EQ(*parsed, input.size());
    }

    TEST(http_head, rejects_malformed_requests) {
        constexpr auto bad_start_line = http::error_code::bad_start_line;
        constexpr auto bad_header = http::error_code::bad_header;

        for (const auto& [input, error] : {
                 std::pair<std::string_view, http::error_code>{"GET  / HTTP/1.1\r\n\r\n", bad_start_line},
                 {"GET / HTTP/1.1\nHost: a\r\n\r\n", bad_start_line},
                 {"GET / HTTP/1.1 \r\n\r\n", bad_start_line},
                 {" GET / HTTP/1.1\r\n\r\n", bad_start_line},
                 {"GET HTTP/1.1\r\n\r\n", bad_start_line},
                 {"G(T / HTTP/1.1\r\n\r\n", bad_start_line},
                 {"GET /a\tb HTTP/1.1\r\n\r\n", bad_start_line},
                 {"GET / http/1.1\r\n\r\n", bad_start_line},
                 {"\r\nGET / HTTP/1.1\r\n\r\n", bad_start_line},
                 {"GET / HTTP/1.2\r\n\r\n", http::error_code::unsupported_version},
                 {"GET / HTTP/1.1\r\nHost: a\n\r\n", bad_header},
                 {"GET / HTTP/1.1\r\nHost: a\r\n\n", bad_header},
                 {"GET / HTTP/1.1\r\nHost: a\rb\r\n\r\n", bad_header},
                 {"GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", bad_header},
                 {"GET / HTTP/1.1\r\nHost : a\r\n\r\n", bad_header},
                 {"GET / HTTP/1.1\r\nHost\r\n\r\n", bad_header},
                 {"GET / HTTP/1.1\r\n: a\r\n\r\n", bad_header},
                 {"GET / HTTP/1.1\r\nHost: a\x7F\r\n\r\n", bad_header},
             }) {
            http::request_head head;
            const auto parsed = h1::parse_head(tests::as_bytes(input), head);

            EXPECT_TRUE(tests::has_error(parsed, error)) << input;
            EXPECT_TRUE(tests::has_error(parsed, std::errc::protocol_error)) << input;
        }
    }

    TEST(http_head, rejects_malformed_status_lines) {
        constexpr auto bad_start_line = http::error_code::bad_start_line;

        for (const auto& [input, error] : {
                 std::pair<std::string_view, http::error_code>{"HTTP/1.1 20 OK\r\n\r\n", bad_start_line},
                 {"HTTP/1.1 2000 OK\r\n\r\n", bad_start_line},
                 {"HTTP/1.1 099 Low\r\n\r\n", bad_start_line},
                 {"HTTP/1.1  200 OK\r\n\r\n", bad_start_line},
                 {"HTTP/1.1\r\n\r\n", bad_start_line},
                 {"HTTP/1.1 200 O\rK\r\n\r\n", bad_start_line},
                 {"ICY 200 OK\r\n\r\n", bad_start_line},
                 {"HTTP/2.0 200 OK\r\n\r\n", http::error_code::unsupported_version},
             }) {
            http::response_head head;

            EXPECT_TRUE(tests::has_error(h1::parse_head(tests::as_bytes(input), head), error)) << input;
        }
    }

    TEST(http_head, enforces_limits) {
        constexpr std::string_view input = "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\n\r\n";
        const auto bytes = tests::as_bytes(input);
        http::request_head head;

        const auto exact_fit = h1::parse_head(bytes, head, {.max_head_size = input.size()});
        ASSERT_TRUE(exact_fit) << exact_fit.error();
        EXPECT_EQ(*exact_fit, input.size());
        EXPECT_TRUE(tests::has_error(h1::parse_head(bytes, head, {.max_head_size = input.size() - 1}),
                                     http::error_code::head_too_large));

        const auto partial = bytes.first(10);
        const auto below_limit = h1::parse_head(partial, head, {.max_head_size = 11});
        ASSERT_TRUE(below_limit) << below_limit.error();
        EXPECT_EQ(*below_limit, std::nullopt);
        EXPECT_TRUE(tests::has_error(h1::parse_head(partial, head, {.max_head_size = 10}),
                                     http::error_code::head_too_large));

        const auto two_headers = h1::parse_head(bytes, head, {.max_headers = 2});
        ASSERT_TRUE(two_headers) << two_headers.error();
        EXPECT_EQ(head.headers.size(), 2uz);
        EXPECT_TRUE(tests::has_error(h1::parse_head(bytes, head, {.max_headers = 1}),
                                     http::error_code::too_many_headers));

        const auto unlimited =
            h1::parse_head(bytes, head, {.max_head_size = std::nullopt, .max_headers = std::nullopt});
        ASSERT_TRUE(unlimited) << unlimited.error();
        EXPECT_EQ(*unlimited, input.size());
    }

    TEST(http_head, written_heads_parse_back) {
        const http::request_head request{
            .method = "POST",
            .target = "/items?id=1",
            .version = http::version::http_1_0,
            .headers = {{.name = "Host", .value = "example.com"}, {.name = "X-Empty", .value = ""}},
        };
        std::vector<std::byte> out;

        const auto written = h1::serialize_head(request, out);
        ASSERT_TRUE(written) << written.error();
        EXPECT_EQ(tests::as_text(out), "POST /items?id=1 HTTP/1.0\r\nHost: example.com\r\nX-Empty: \r\n\r\n");

        http::request_head parsed;
        const auto size = h1::parse_head(out, parsed);
        ASSERT_TRUE(size) << size.error();
        EXPECT_EQ(*size, out.size());
        EXPECT_EQ(parsed.method, request.method);
        EXPECT_EQ(parsed.target, request.target);
        EXPECT_EQ(parsed.version, request.version);
        EXPECT_EQ(parsed.headers, request.headers);
    }

    TEST(http_head, response_reason_defaults_to_the_registered_phrase) {
        for (const auto& [response, expected] : {
                 std::pair<http::response_head, std::string_view>{
                     {.status = http::status::not_found},
                     "HTTP/1.1 404 Not Found\r\n\r\n",
                 },
                 {{.status = http::status::not_found, .reason = "Nope"}, "HTTP/1.1 404 Nope\r\n\r\n"},
                 // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                 {{.status = http::status{299}}, "HTTP/1.1 299 \r\n\r\n"},
             }) {
            std::vector<std::byte> out;

            const auto written = h1::serialize_head(response, out);
            ASSERT_TRUE(written) << written.error();
            EXPECT_EQ(tests::as_text(out), expected);
        }
    }

    TEST(http_head, write_rejects_text_that_would_break_the_message) {
        constexpr std::string_view existing = "already written";
        const auto existing_bytes = tests::as_bytes(existing);
        std::vector<std::byte> out{existing_bytes.begin(), existing_bytes.end()};

        for (const auto& request : {
                 http::request_head{.method = "GE T", .target = "/"},
                 http::request_head{.method = "", .target = "/"},
                 http::request_head{.method = "GET", .target = "/a b"},
                 http::request_head{.method = "GET", .target = ""},
                 // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                 http::request_head{.method = "GET", .target = "/", .version = http::version{2}},
             }) {
            EXPECT_TRUE(
                tests::has_error(h1::serialize_head(request, out), core::error_code::invalid_argument))
                << request.method << ' ' << request.target;
        }

        for (const auto& response : {
                 // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
                 http::response_head{.status = http::status{99}},
                 http::response_head{.status = http::status{1000}},
                 http::response_head{.reason = "OK\r\nEvil: 1"},
                 http::response_head{.version = http::version{2}},
                 // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
             }) {
            EXPECT_TRUE(
                tests::has_error(h1::serialize_head(response, out), core::error_code::invalid_argument))
                << std::to_underlying(response.status) << ' ' << response.reason;
        }

        for (const auto& header : {
                 http::header{.name = "Na me", .value = "a"},
                 http::header{.name = "Name:", .value = "a"},
                 http::header{.name = "", .value = "a"},
                 http::header{.name = "Name", .value = "a\r\nEvil: 1"},
                 http::header{.name = "Name", .value = "a\nb"},
             }) {
            const http::request_head request{.method = "GET", .target = "/", .headers = {header}};
            const http::response_head response{.headers = {header}};

            EXPECT_TRUE(
                tests::has_error(h1::serialize_head(request, out), core::error_code::invalid_argument))
                << header.name << ": " << header.value;
            EXPECT_TRUE(
                tests::has_error(h1::serialize_head(response, out), core::error_code::invalid_argument))
                << header.name << ": " << header.value;
        }

        EXPECT_EQ(tests::as_text(out), existing);
    }
} // namespace rei::net::tests
