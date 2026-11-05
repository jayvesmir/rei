#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/core/url/idna.hpp"

namespace rei::core::tests {
    namespace {
        auto trim(std::string_view text) -> std::string_view {
            auto first = text.find_first_not_of(" \t");
            if (first == std::string_view::npos) {
                return {};
            }

            return text.substr(first, text.find_last_not_of(" \t") - first + 1);
        }

        auto append_utf8(std::string& text, char32_t code_point) -> void {
            if (code_point < 0x80) {
                text += static_cast<char>(code_point);
            } else if (code_point < 0x800) {
                text += static_cast<char>(0xC0 | (code_point >> 6));
                text += static_cast<char>(0x80 | (code_point & 0x3F));
            } else if (code_point < 0x10000) {
                text += static_cast<char>(0xE0 | (code_point >> 12));
                text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
                text += static_cast<char>(0x80 | (code_point & 0x3F));
            } else {
                text += static_cast<char>(0xF0 | (code_point >> 18));
                text += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
                text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
                text += static_cast<char>(0x80 | (code_point & 0x3F));
            }
        }

        // Resolves the \uXXXX and \x{XXXX} escapes and reads "" as the empty string. Escaped surrogates can't
        // be UTF-8, so they give std::nullopt.
        auto unescape(std::string_view column) -> std::optional<std::string> {
            if (column == "\"\"") {
                return std::string{};
            }

            std::string text;
            for (auto i = 0uz; i < column.size(); i++) {
                std::string_view hex;
                if (column.substr(i).starts_with("\\u")) {
                    hex = column.substr(i + 2, 4);
                    i += 5;
                } else if (column.substr(i).starts_with("\\x{")) {
                    auto end = column.find('}', i);
                    hex = column.substr(i + 3, end - i - 3);
                    i = end;
                } else {
                    text += column[i];
                    continue;
                }

                std::uint32_t code_point{};
                std::from_chars(hex.data(), hex.data() + hex.size(), code_point, 16);
                if (code_point >= 0xD800 && code_point <= 0xDFFF) {
                    return std::nullopt;
                }

                append_utf8(text, static_cast<char32_t>(code_point));
            }

            return text;
        }

        // Ignores the codes of the flags the URL standard turns off: VerifyDnsLength (A4_1, A4_2),
        // CheckHyphens (V2, V3) and UseSTD3ASCIIRules (U1).
        auto has_error(std::string_view status) -> bool {
            status = trim(status);
            if (status.starts_with('[') && status.ends_with(']')) {
                status = status.substr(1, status.size() - 2);
            }

            return std::ranges::any_of(status | std::views::split(','), [](auto code) -> bool {
                auto name = trim(std::string_view{code});
                return !name.empty() && name != "A4_1" && name != "A4_2" && name != "V2" && name != "V3" &&
                       name != "U1";
            });
        }
    } // namespace

    TEST(idna, to_ascii_matches_unicode_test_data) {
        const char* path = std::getenv("IDNA_TEST_V2");
        ASSERT_NE(path, nullptr) << "IDNA_TEST_V2 should point to IdnaTestV2.txt";

        std::ifstream file{path};
        ASSERT_TRUE(file.is_open()) << path;

        auto cases = 0;
        for (std::string line; std::getline(file, line);) {
            auto data = std::string_view{line}.substr(0, line.find('#'));
            if (trim(data).empty()) {
                continue;
            }

            // Blank columns default to an earlier one: toUnicode to the source, toAsciiN to toUnicode and
            // toAsciiNStatus to toUnicodeStatus.
            std::vector<std::string_view> columns;
            for (auto column : data | std::views::split(';')) {
                columns.push_back(trim(std::string_view{column}));
            }

            auto to_unicode = columns.at(1).empty() ? columns.at(0) : columns.at(1);
            auto to_ascii = columns.at(3).empty() ? to_unicode : columns.at(3);
            auto status = columns.at(4).empty() ? columns.at(2) : columns.at(4);

            auto source = tests::unescape(columns.at(0));
            auto expected = tests::unescape(to_ascii);
            if (!source || !expected) {
                continue;
            }

            auto result = detail::idna::to_ascii(*source);
            if (tests::has_error(status)) {
                EXPECT_FALSE(result) << line << "\n  gave " << *result;
            } else if (!result) {
                ADD_FAILURE() << line << "\n  failed with " << result.error();
            } else {
                EXPECT_EQ(*result, *expected) << line;
            }

            cases++;
        }

        // Guards against a file that parses to nothing.
        EXPECT_GT(cases, 6000);
    }
} // namespace rei::core::tests
