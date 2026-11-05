#include <string_view>

#include "gtest/gtest.h"

#include "rei/core/utf8.hpp"

namespace rei::core::tests {
    TEST(utf8, accepts_every_sequence_length_up_to_its_limits) {
        for (std::string_view text : {
                 "",
                 "ascii\x7F",
                 "\xC2\x80", // U+0080
                 "\xDF\xBF", // U+07FF
                 "\xE0\xA0\x80", // U+0800
                 "\xED\x9F\xBF", // U+D7FF, just below the surrogates
                 "\xEE\x80\x80", // U+E000, just above them
                 "\xEF\xBF\xBF", // U+FFFF
                 "\xF0\x90\x80\x80", // U+10000
                 "\xF4\x8F\xBF\xBF", // U+10FFFF
             }) {
            EXPECT_TRUE(core::utf8::is_valid(text)) << testing::PrintToString(text);
        }
    }

    TEST(utf8, rejects_ill_formed_sequences) {
        for (std::string_view text : {
                 "\x80", // continuation without a lead
                 "\xC0\x80", // overlong U+0000
                 "\xE0\x9F\xBF", // overlong U+07FF
                 "\xF0\x8F\xBF\xBF", // overlong U+FFFF
                 "\xED\xA0\x80", // U+D800
                 "\xED\xBF\xBF", // U+DFFF
                 "\xF4\x90\x80\x80", // U+110000
                 "\xF5\x80\x80\x80",
                 "\xFF",
                 "a\xE2\x82", // truncated at the end
                 "\xC3(", // ASCII where a continuation belongs
             }) {
            EXPECT_FALSE(core::utf8::is_valid(text)) << testing::PrintToString(text);
        }
    }
} // namespace rei::core::tests
