#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "rei/net/http.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    namespace h1 = http::h1;

    TEST(http_framing, request_body_follows_the_length_headers) {
        constexpr auto max_size = std::numeric_limits<std::uint64_t>::max();

        for (const auto& [headers, expected] : {
                 std::pair<std::string_view, h1::body_framing>{"", {.kind = h1::body_kind::none}},
                 {"content-length: 42\r\n", {.kind = h1::body_kind::sized, .size = 42}},
                 {"Content-Length: 0\r\n", {.kind = h1::body_kind::sized, .size = 0}},
                 {
                     "Content-Length: 18446744073709551615\r\n",
                     {.kind = h1::body_kind::sized, .size = max_size},
                 },
                 {"Transfer-Encoding: Chunked\r\n", {.kind = h1::body_kind::chunked}},
             }) {
            const auto text = "POST / HTTP/1.1\r\n" + std::string{headers} + "\r\n";
            http::request_head head;
            const auto parsed = h1::parse_head(tests::as_bytes(text), head);
            ASSERT_TRUE(parsed && *parsed) << headers;

            const auto framing = h1::request_framing(head);
            ASSERT_TRUE(framing) << headers << framing.error();
            EXPECT_EQ(*framing, expected) << headers;
        }
    }

    TEST(http_framing, rejects_lengths_that_could_be_read_two_ways) {
        struct framing_case {
            std::string_view version{"HTTP/1.1"};
            std::string_view headers;
            http::error_code error;
        };

        constexpr auto bad_framing = http::error_code::bad_framing;
        constexpr auto unsupported = http::error_code::unsupported_transfer_coding;

        for (const auto& [version, headers, error] : {
                 framing_case{
                     .headers = "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n",
                     .error = bad_framing,
                 },
                 framing_case{.headers = "Content-Length: 1\r\ncontent-length: 1\r\n", .error = bad_framing},
                 framing_case{.headers = "Content-Length:\r\n", .error = bad_framing},
                 framing_case{.headers = "Content-Length: 1, 1\r\n", .error = bad_framing},
                 framing_case{.headers = "Content-Length: -1\r\n", .error = bad_framing},
                 framing_case{.headers = "Content-Length: +1\r\n", .error = bad_framing},
                 framing_case{.headers = "Content-Length: 0x10\r\n", .error = bad_framing},
                 framing_case{.headers = "Content-Length: 18446744073709551616\r\n", .error = bad_framing},
                 framing_case{
                     .version = "HTTP/1.0",
                     .headers = "Transfer-Encoding: chunked\r\n",
                     .error = bad_framing,
                 },
                 framing_case{.headers = "Transfer-Encoding: gzip\r\n", .error = bad_framing},
                 framing_case{.headers = "Transfer-Encoding: chunked, gzip\r\n", .error = bad_framing},
                 framing_case{
                     .headers = "Transfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n",
                     .error = bad_framing,
                 },
                 framing_case{.headers = "Transfer-Encoding: gzip, chunked\r\n", .error = unsupported},
                 framing_case{
                     .headers = "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
                     .error = unsupported,
                 },
             }) {
            const auto request_text =
                "POST / " + std::string{version} + "\r\n" + std::string{headers} + "\r\n";
            http::request_head request;
            const auto parsed_request = h1::parse_head(tests::as_bytes(request_text), request);
            ASSERT_TRUE(parsed_request && *parsed_request) << headers;

            const auto response_text = std::string{version} + " 200 OK\r\n" + std::string{headers} + "\r\n";
            http::response_head response;
            const auto parsed_response = h1::parse_head(tests::as_bytes(response_text), response);
            ASSERT_TRUE(parsed_response && *parsed_response) << headers;

            EXPECT_TRUE(tests::has_error(h1::request_framing(request), error)) << headers;
            EXPECT_TRUE(tests::has_error(h1::response_framing(response, "GET"), error)) << headers;
        }
    }

    TEST(http_framing, response_body_depends_on_the_request_method_and_status) {
        struct framing_case {
            std::string_view request_method;
            std::string_view head;
            h1::body_framing expected;
        };

        constexpr h1::body_framing none{.kind = h1::body_kind::none};
        constexpr h1::body_framing five_bytes{.kind = h1::body_kind::sized, .size = 5};

        for (const auto& [request_method, head_text, expected] : {
                 framing_case{
                     .request_method = "GET",
                     .head = "HTTP/1.1 200 OK\r\n\r\n",
                     .expected = {.kind = h1::body_kind::until_close},
                 },
                 framing_case{
                     .request_method = "GET",
                     .head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                     .expected = five_bytes,
                 },
                 framing_case{
                     .request_method = "GET",
                     .head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                     .expected = {.kind = h1::body_kind::chunked},
                 },
                 framing_case{
                     .request_method = "HEAD",
                     .head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                     .expected = none,
                 },
                 framing_case{
                     .request_method = "GET",
                     .head = "HTTP/1.1 100 Continue\r\n\r\n",
                     .expected = none,
                 },
                 framing_case{
                     .request_method = "GET",
                     .head = "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n",
                     .expected = none,
                 },
                 framing_case{
                     .request_method = "GET",
                     .head = "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\n",
                     .expected = none,
                 },
                 framing_case{
                     .request_method = "CONNECT",
                     .head = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                     .expected = none,
                 },
                 framing_case{
                     .request_method = "CONNECT",
                     .head = "HTTP/1.1 403 Forbidden\r\nContent-Length: 5\r\n\r\n",
                     .expected = five_bytes,
                 },
             }) {
            http::response_head head;
            const auto parsed = h1::parse_head(tests::as_bytes(head_text), head);
            ASSERT_TRUE(parsed && *parsed) << head_text;

            const auto framing = h1::response_framing(head, request_method);
            ASSERT_TRUE(framing) << head_text << framing.error();
            EXPECT_EQ(*framing, expected) << request_method << " answered with " << head_text;
        }
    }

    TEST(http_framing, keeps_alive_follows_the_version_and_connection_options) {
        for (const auto& [head_text, expected] : {
                 std::pair<std::string_view, bool>{"GET / HTTP/1.1\r\n\r\n", true},
                 {"GET / HTTP/1.1\r\nConnection: close\r\n\r\n", false},
                 {"GET / HTTP/1.1\r\nconnection: Upgrade ,\tClose\r\n\r\n", false},
                 {"GET / HTTP/1.1\r\nConnection: closed\r\n\r\n", true},
                 {"GET / HTTP/1.0\r\n\r\n", false},
                 {"GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", true},
                 {"GET / HTTP/1.0\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n", false},
             }) {
            http::request_head head;
            const auto parsed = h1::parse_head(tests::as_bytes(head_text), head);
            ASSERT_TRUE(parsed && *parsed) << head_text;

            EXPECT_EQ(h1::keeps_alive(head.version, head.headers), expected) << head_text;
        }
    }
} // namespace rei::net::tests
