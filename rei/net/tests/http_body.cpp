#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/net/http/error_code.hpp"
#include "rei/net/http/h1.hpp"
#include "rei/net/tests/support.hpp"

namespace rei::net::tests {
    namespace h1 = http::h1;

    namespace {
        struct decoded_body {
            std::string payload;
            std::size_t consumed{};
            bool done{};
        };

        auto decode_in_pieces(h1::body_framing framing, std::string_view input, std::size_t piece_size)
            -> std::expected<decoded_body, rei::error> {
            h1::body_decoder decoder{framing};
            decoded_body decoded;

            while (decoded.consumed < input.size() && !decoder.done()) {
                auto piece = tests::as_bytes(input.substr(decoded.consumed, piece_size));

                while (!piece.empty() && !decoder.done()) {
                    const auto slice = decoder.decode(piece);
                    if (!slice) {
                        return std::unexpected(slice.error());
                    }

                    decoded.payload += tests::as_text(slice->data);
                    decoded.consumed += slice->consumed;
                    piece = piece.subspan(slice->consumed);
                }
            }

            decoded.done = decoder.done();
            return decoded;
        }
    } // namespace

    TEST(http_body, sized_body_ends_at_its_length) {
        constexpr std::string_view input = "helloGET /next";
        h1::body_decoder decoder{{.kind = h1::body_kind::sized, .size = 5}};

        auto slice = decoder.decode(tests::as_bytes(input.substr(0, 2)));
        ASSERT_TRUE(slice) << slice.error();
        EXPECT_EQ(slice->consumed, 2uz);
        EXPECT_EQ(tests::as_text(slice->data), "he");
        EXPECT_EQ(slice->data.data(), tests::as_bytes(input).data());
        EXPECT_FALSE(decoder.done());

        slice = decoder.decode(tests::as_bytes(input.substr(2)));
        ASSERT_TRUE(slice) << slice.error();
        EXPECT_EQ(slice->consumed, 3uz);
        EXPECT_EQ(tests::as_text(slice->data), "llo");
        EXPECT_TRUE(decoder.done());

        slice = decoder.decode(tests::as_bytes(input.substr(5)));
        ASSERT_TRUE(slice) << slice.error();
        EXPECT_EQ(slice->consumed, 0uz);
        EXPECT_TRUE(slice->data.empty());

        EXPECT_TRUE(h1::body_decoder{{.kind = h1::body_kind::none}}.done());
        EXPECT_TRUE((h1::body_decoder{{.kind = h1::body_kind::sized, .size = 0}}.done()));
    }

    TEST(http_body, until_close_body_takes_everything_and_never_finishes) {
        constexpr std::string_view input = "0\r\n\r\nstill the body";
        h1::body_decoder decoder{{.kind = h1::body_kind::until_close}};

        const auto slice = decoder.decode(tests::as_bytes(input));
        ASSERT_TRUE(slice) << slice.error();
        EXPECT_EQ(slice->consumed, input.size());
        EXPECT_EQ(tests::as_text(slice->data), input);
        EXPECT_FALSE(decoder.done());
    }

    TEST(http_body, chunked_body_decodes_the_same_however_it_is_split) {
        constexpr std::string_view body = "5\r\nhello\r\n"
                                          "1 ;name=value; other=\"quoted\"\r\n \r\n"
                                          "00B \t\r\nworld again\r\n"
                                          "0\r\n"
                                          "Checksum: abc\r\n"
                                          "Other: 1\r\n"
                                          "\r\n";
        const auto input = std::string{body} + "HTTP/1.1 200 OK\r\n";

        for (auto piece_size = 1uz; piece_size <= input.size(); piece_size++) {
            const auto decoded = tests::decode_in_pieces({.kind = h1::body_kind::chunked}, input, piece_size);

            ASSERT_TRUE(decoded) << piece_size << ": " << decoded.error();
            EXPECT_EQ(decoded->payload, "hello world again") << piece_size;
            EXPECT_EQ(decoded->consumed, body.size()) << piece_size;
            EXPECT_TRUE(decoded->done) << piece_size;
        }
    }

    TEST(http_body, rejects_malformed_chunks) {
        for (const std::string_view input : {
                 "\r\nhello\r\n",
                 "g\r\n",
                 "-5\r\nhello\r\n",
                 "0x5\r\nhello\r\n",
                 "5\nhello\r\n",
                 "5\r\rhello\r\n",
                 "5;name\nhello\r\n",
                 "5 6\r\nhello\r\n",
                 "5\r\nhello0\r\n\r\n",
                 "5\r\nhello\n0\r\n\r\n",
                 "10000000000000000\r\n",
                 "0\r\nChecksum: abc\n\r\n",
                 "0\r\n\rX",
             }) {
            const auto decoded =
                tests::decode_in_pieces({.kind = h1::body_kind::chunked}, input, input.size());

            EXPECT_TRUE(tests::has_error(decoded, http::error_code::bad_chunk)) << input;
        }

        h1::body_decoder decoder{{.kind = h1::body_kind::chunked}};
        EXPECT_TRUE(
            tests::has_error(decoder.decode(tests::as_bytes("0\r\n\rX")), http::error_code::bad_chunk));
        EXPECT_FALSE(decoder.done());
        EXPECT_TRUE(tests::has_error(decoder.decode(tests::as_bytes("\r\n")), http::error_code::bad_chunk));
    }

    TEST(http_body, limits_discarded_extensions_and_trailers) {
        constexpr auto limit = 16uz * 1024;
        const auto with_extension = [](std::size_t size) -> std::string {
            return "5;" + std::string(size - 1, 'a') + "\r\nhello\r\n0\r\n\r\n";
        };

        const auto at_limit = with_extension(limit);
        const auto decoded =
            tests::decode_in_pieces({.kind = h1::body_kind::chunked}, at_limit, at_limit.size());
        ASSERT_TRUE(decoded) << decoded.error();
        EXPECT_EQ(decoded->payload, "hello");
        EXPECT_TRUE(decoded->done);

        // The limit is per body, not per chunk.
        const auto over_limit = with_extension(limit + 1);
        const auto with_trailer = with_extension(limit).insert(at_limit.size() - 2, "A: 1\r\n");
        for (const auto& input : {over_limit, with_trailer}) {
            EXPECT_TRUE(tests::has_error(
                tests::decode_in_pieces({.kind = h1::body_kind::chunked}, input, input.size()),
                http::error_code::bad_chunk));
        }
    }

    TEST(http_body, written_chunks_decode_back) {
        const std::string large(300, 'x');
        std::vector<std::byte> out;

        h1::write_chunk(tests::as_bytes("hello "), out);
        h1::write_chunk({}, out);
        h1::write_chunk(tests::as_bytes(large), out);
        h1::write_last_chunk(out);
        EXPECT_EQ(tests::as_text(out), "6\r\nhello \r\n12c\r\n" + large + "\r\n0\r\n\r\n");

        const auto decoded =
            tests::decode_in_pieces({.kind = h1::body_kind::chunked}, tests::as_text(out), 7);
        ASSERT_TRUE(decoded) << decoded.error();
        EXPECT_EQ(decoded->payload, "hello " + large);
        EXPECT_EQ(decoded->consumed, out.size());
        EXPECT_TRUE(decoded->done);
    }
} // namespace rei::net::tests
