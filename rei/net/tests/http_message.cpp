#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/net/http.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    TEST(http_message, headers_keep_order_and_duplicates) {
        http::headers headers{
            {.name = "Set-Cookie", .value = "a=1"},
            {.name = "Content-Type", .value = "text/plain"},
            {.name = "set-cookie", .value = "b=2"},
        };
        EXPECT_EQ(headers.find("SET-COOKIE"), "a=1");
        EXPECT_FALSE(headers.find("Accept"));

        headers.add("Accept", "*/*");
        headers.set("Set-Cookie", "c=3");
        headers.set("Server", "rei");
        headers.remove("content-type");

        const std::vector<http::headers::field> fields(headers.begin(), headers.end());
        EXPECT_EQ(fields, (std::vector<http::headers::field>{
                              {"Set-Cookie", "c=3"},
                              {"Accept", "*/*"},
                              {"Server", "rei"},
                          }));
    }

    TEST(http_message, body_holds_bytes_or_a_reader) {
        http::body text = "hello";
        EXPECT_FALSE(text.streamed());
        EXPECT_EQ(text.size(), 5u);
        EXPECT_EQ(text.text(), "hello");

        std::array<std::byte, 3> out{};
        auto read = text.read(out);
        ASSERT_TRUE(read) << read.error();
        EXPECT_EQ(tests::as_text(std::span{out}.first(*read)), "hel");
        read = text.read(out);
        ASSERT_TRUE(read) << read.error();
        EXPECT_EQ(tests::as_text(std::span{out}.first(*read)), "lo");
        EXPECT_EQ(text.read(out), 0u);

        auto produced = false;
        http::body reader{[&produced](std::span<std::byte> out) -> std::expected<std::size_t, rei::error> {
            if (produced) {
                return 0;
            }

            produced = true;
            out[0] = std::byte{'x'};
            return 1;
        }};
        EXPECT_TRUE(reader.streamed());
        EXPECT_FALSE(reader.size());
        EXPECT_TRUE(reader.text().empty());
        EXPECT_EQ(reader.read(out), 1u);
        EXPECT_EQ(reader.read(out), 0u);

        EXPECT_EQ(http::body{}.size(), 0u);
    }
} // namespace rei::net::tests
