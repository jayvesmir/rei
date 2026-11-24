#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/formats/json.hpp"

namespace rei::formats::tests {
    namespace {
        auto read(const std::filesystem::path& path) -> std::string {
            std::ifstream file{path, std::ios::binary};
            return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        auto cases(std::string_view prefix) -> std::vector<std::filesystem::path> {
            const char* anchor = std::getenv("JSON_TEST_SUITE");
            if (anchor == nullptr) {
                return {};
            }

            std::vector<std::filesystem::path> found;
            for (const auto& entry :
                 std::filesystem::directory_iterator{std::filesystem::path{anchor}.parent_path()}) {
                if (entry.path().filename().string().starts_with(prefix)) {
                    found.push_back(entry.path());
                }
            }

            return found;
        }
    } // namespace

    TEST(json_test, parses_every_valid_document_and_writes_it_back) {
        const auto valid = tests::cases("y_");
        ASSERT_EQ(valid.size(), 95U) << "JSON_TEST_SUITE should point into JSONTestSuite's test_parsing";

        for (const auto& path : valid) {
            const auto document = json::parse(tests::read(path));
            if (!document) {
                ADD_FAILURE() << path << "\n  " << document.error();
                continue;
            }

            const auto text = json::serialize(*document);
            ASSERT_TRUE(text) << path << "\n  " << text.error();

            const auto read_back = json::parse(*text);
            ASSERT_TRUE(read_back) << path << "\n  " << read_back.error() << "\n" << *text;
            EXPECT_EQ(*read_back, *document) << path << "\n" << *text;
        }
    }

    TEST(json_test, rejects_every_invalid_document) {
        const auto invalid = tests::cases("n_");
        ASSERT_EQ(invalid.size(), 188U) << "JSON_TEST_SUITE should point into JSONTestSuite's test_parsing";

        for (const auto& path : invalid) {
            EXPECT_FALSE(json::parse(tests::read(path))) << path;
        }

        for (const auto& path : tests::cases("i_")) {
            if (const auto document = json::parse(tests::read(path))) {
                EXPECT_TRUE(json::serialize(*document)) << path;
            }
        }
    }
} // namespace rei::formats::tests
