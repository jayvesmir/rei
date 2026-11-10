#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/net/error_code.hpp"
#include "rei/net/http.hpp"
#include "rei/net/http/connection.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    using namespace std::chrono_literals;

    namespace {
        auto read_whole_body(http::connection& connection, std::size_t piece_size)
            -> std::expected<std::string, rei::error> {
            std::array<std::byte, 64> buffer{};
            const auto out = std::span{buffer}.first(piece_size);
            std::string body;

            while (true) {
                const auto read = connection.read_body(out);
                if (!read) {
                    return std::unexpected(read.error());
                }
                if (*read == 0) {
                    return body;
                }

                body += tests::as_text(out.first(*read));
            }
        }
    } // namespace

    TEST(http_connection, reads_pipelined_requests_and_keeps_head_views_through_the_body) {
        constexpr std::string_view input = "POST /upload HTTP/1.1\r\n"
                                           "Host: a\r\n"
                                           "Transfer-Encoding: chunked\r\n"
                                           "\r\n"
                                           "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"
                                           "GET /next HTTP/1.1\r\n"
                                           "Host: b\r\n"
                                           "Content-Length: 3\r\n"
                                           "\r\n"
                                           "abc";

        auto pair = tests::make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        ASSERT_EQ(pair->client.send(tests::as_bytes(input)), input.size());

        http::connection connection{std::move(pair->accepted)};
        http::request_head head;

        auto received = connection.read_head(head);
        ASSERT_TRUE(received) << received.error();
        EXPECT_FALSE(connection.reusable());

        auto body = read_whole_body(connection, 4);
        ASSERT_TRUE(body) << body.error();
        EXPECT_EQ(*body, "hello world");
        EXPECT_EQ(head.target, "/upload");
        EXPECT_EQ(http::find_header(head.headers, "host"), "a");

        received = connection.read_head(head);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(head.target, "/next");

        body = read_whole_body(connection, 64);
        ASSERT_TRUE(body) << body.error();
        EXPECT_EQ(*body, "abc");
        EXPECT_TRUE(connection.reusable());
    }

    TEST(http_connection, grows_its_buffer_for_a_large_head_and_keeps_what_follows) {
        // More than the buffer starts with, so the head's bytes move while they arrive.
        const auto cookie = std::string(40uz * 1024, 'c');
        const auto input =
            std::format("POST /upload HTTP/1.1\r\nCookie: {}\r\nContent-Length: 5\r\n\r\nhello", cookie);

        auto pair = tests::make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        ASSERT_EQ(pair->client.send(tests::as_bytes(input)), input.size());

        // The largest limit stands for none.
        http::connection connection{std::move(pair->accepted),
                                    {.max_head_size = std::numeric_limits<std::size_t>::max()}};
        http::request_head head;

        const auto received = connection.read_head(head);
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(head.target, "/upload");
        EXPECT_EQ(http::find_header(head.headers, "cookie"), cookie);

        const auto body = read_whole_body(connection, 64);
        ASSERT_TRUE(body) << body.error();
        EXPECT_EQ(*body, "hello");
    }

    TEST(http_connection, writes_bodies_that_match_their_framing) {
        auto pair = tests::make_tcp_pair();
        ASSERT_TRUE(pair) << pair.error();
        http::connection connection{std::move(pair->accepted)};

        const http::response_head sized{.headers = {{.name = "Content-Length", .value = "5"}}};
        ASSERT_TRUE(connection.write_head(sized, "GET"));
        EXPECT_TRUE(tests::has_error(connection.write_body(tests::as_bytes("too long")),
                                     std::errc::invalid_argument));
        ASSERT_TRUE(connection.write_body(tests::as_bytes("hel")));
        ASSERT_TRUE(connection.write_body(tests::as_bytes("lo")));
        ASSERT_TRUE(connection.finish());

        const http::response_head chunked{.headers = {{.name = "Transfer-Encoding", .value = "chunked"}}};
        ASSERT_TRUE(connection.write_head(chunked, "GET"));
        ASSERT_TRUE(connection.write_body(tests::as_bytes("ab")));
        ASSERT_TRUE(connection.write_body(tests::as_bytes("cd")));
        ASSERT_TRUE(connection.finish());

        ASSERT_TRUE(connection.write_head(sized, "HEAD"));
        EXPECT_TRUE(
            tests::has_error(connection.write_body(tests::as_bytes("x")), std::errc::invalid_argument));
        ASSERT_TRUE(connection.finish());
        EXPECT_TRUE(connection.reusable());

        constexpr std::string_view expected = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
                                              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                              "2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n"
                                              "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        const auto received = tests::receive_exactly(pair->client, expected.size());
        ASSERT_TRUE(received) << received.error();
        EXPECT_EQ(*received, expected);

        ASSERT_TRUE(connection.write_head(sized, "GET"));
        ASSERT_TRUE(connection.write_body(tests::as_bytes("hi")));
        EXPECT_TRUE(tests::has_error(connection.finish(), std::errc::invalid_argument));
        EXPECT_FALSE(connection.reusable());
    }

    TEST(http_connection, ends_bodies_at_deadlines_and_at_the_peer_closing) {
        auto idle = tests::make_tcp_pair();
        ASSERT_TRUE(idle) << idle.error();
        http::connection waiting{std::move(idle->accepted)};
        http::request_head request;

        waiting.deadline(std::chrono::steady_clock::now() + 50ms);
        EXPECT_TRUE(tests::has_error(waiting.read_head(request), net::error_code::timed_out));
        EXPECT_FALSE(waiting.reusable());

        const auto read_response = [](std::string_view input) -> std::expected<std::string, rei::error> {
            auto pair = tests::make_tcp_pair();
            if (!pair) {
                return std::unexpected(pair.error());
            }

            http::connection connection{std::move(pair->client)};
            if (auto sent = pair->accepted.send(tests::as_bytes(input)); !sent) {
                return std::unexpected(sent.error());
            }
            (void)pair->accepted.close();

            http::response_head head;
            auto body =
                connection.read_head(head, "GET").and_then([&] -> std::expected<std::string, rei::error> {
                    return read_whole_body(connection, 64);
                });
            EXPECT_FALSE(connection.reusable());

            return body;
        };

        const auto until_close = read_response("HTTP/1.0 200 OK\r\n\r\nuntil the peer closes");
        ASSERT_TRUE(until_close) << until_close.error();
        EXPECT_EQ(*until_close, "until the peer closes");

        EXPECT_TRUE(tests::has_error(read_response("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"),
                                     net::error_code::connection_closed));
    }
} // namespace rei::net::tests
