#include <expected>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/core/error.hpp"
#include "rei/core/url.hpp"

namespace rei::core::tests {
    static_assert(!std::is_default_constructible_v<core::url>); // Has to pass validation
    static_assert(std::is_trivially_copyable_v<core::url_view>); // It's literally just a pile of string views

    template <typename Value>
    auto has_error(const std::expected<Value, rei::error>& result, std::errc expected)
        -> testing::AssertionResult {
        if (result) {
            return testing::AssertionFailure() << "operation succeeded";
        }
        if (result.error().code() != expected) {
            return testing::AssertionFailure() << "unexpected error " << result.error();
        }

        return testing::AssertionSuccess();
    }

    TEST(url, parsing_normalizes_every_component) {
        auto parsed = core::url::make("\t HTTPS://User:Pa ss@EXAM\nPLE.com:443/a/./b/../c d?q=1 2'#fr\tag`\n");
        ASSERT_TRUE(parsed) << parsed.error();

        EXPECT_EQ(parsed->scheme(), "https");
        EXPECT_EQ(parsed->username(), "User");
        EXPECT_EQ(parsed->password(), "Pa%20ss");
        EXPECT_EQ(parsed->host(), "example.com");
        EXPECT_EQ(parsed->port(), std::nullopt);
        EXPECT_EQ(parsed->path(), "/a/c%20d");
        EXPECT_EQ(parsed->query(), "q=1%202%27");
        EXPECT_EQ(parsed->fragment(), "frag%60");
        EXPECT_EQ(parsed->serialize(), "https://User:Pa%20ss@example.com/a/c%20d?q=1%202%27#frag%60");
    }

    TEST(url, hosts_are_parsed_according_to_the_scheme) {
        struct host_case {
            std::string_view input;
            std::optional<std::string_view> host;
            std::string_view href;
        };

        for (const auto& [input, host, href] : {
                 host_case{.input = "http://0x7F.1/", .host = "127.0.0.1", .href = "http://127.0.0.1/"},
                 host_case{
                     .input = "http://M\xC3\xBCnchen.DE/",
                     .host = "xn--mnchen-3ya.de",
                     .href = "http://xn--mnchen-3ya.de/",
                 },
                 // IDNA maps full-width digits to ASCII before the IPv4 check.
                 host_case{
                     .input = "http://\xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x97.0.0.1/",
                     .host = "127.0.0.1",
                     .href = "http://127.0.0.1/",
                 },
                 // ASCII domains are only lowercased, even with invalid Punycode.
                 host_case{.input = "http://XN--A/", .host = "xn--a", .href = "http://xn--a/"},
                 host_case{
                     .input = "http://[0:0:0:0:0:FFFF:192.168.0.1]/",
                     .host = "[::ffff:c0a8:1]",
                     .href = "http://[::ffff:c0a8:1]/",
                 },
                 host_case{.input = "sc://Ex%41mple/", .host = "Ex%41mple", .href = "sc://Ex%41mple/"},
                 host_case{.input = "file://localhost/x", .host = "", .href = "file:///x"},
                 host_case{.input = "foo://host", .host = "host", .href = "foo://host"},
                 host_case{.input = "mailto:a@b", .host = std::nullopt, .href = "mailto:a@b"},
             }) {
            auto parsed = core::url::make(input);
            ASSERT_TRUE(parsed) << input << ": " << parsed.error();
            EXPECT_EQ(parsed->host(), host) << input;
            EXPECT_EQ(parsed->serialize(), href);
        }
    }

    TEST(url, relative_references_resolve_against_a_base) {
        auto base = core::url::make("https://example.com/a/b/c?x#y");
        ASSERT_TRUE(base) << base.error();

        for (const auto& [input, href] : {
                 std::pair<std::string_view, std::string_view>{"../d", "https://example.com/a/d"},
                 {"?z", "https://example.com/a/b/c?z"},
                 {"//other.org/p", "https://other.org/p"},
                 {"", "https://example.com/a/b/c?x"},
             }) {
            auto resolved = core::url::make(input, *base);
            ASSERT_TRUE(resolved) << input << ": " << resolved.error();
            EXPECT_EQ(resolved->serialize(), href);
        }

        auto opaque = core::url_view::make("mailto:x");
        ASSERT_TRUE(opaque) << opaque.error();

        auto fragment = core::url::make("#f", *opaque);
        ASSERT_TRUE(fragment) << fragment.error();
        EXPECT_EQ(fragment->serialize(), "mailto:x#f");
        EXPECT_TRUE(tests::has_error(core::url::make("f", *opaque), std::errc::invalid_argument));

        // The Windows drive letter quirk keeps the drive when resolving an absolute path.
        auto drive = core::url::make("file:///C:/dir/file");
        ASSERT_TRUE(drive) << drive.error();

        auto absolute = core::url::make("/x", *drive);
        ASSERT_TRUE(absolute) << absolute.error();
        EXPECT_EQ(absolute->serialize(), "file:///C:/x");
    }

    TEST(url, invalid_input_is_rejected) {
        for (std::string_view input : {
                 "",
                 "example.com",
                 "https://",
                 "https://exa mple.com/",
                 "http://[::1",
                 "http://1.2.3.256/",
                 "http://example.com:65536/",
                 "http://user@/",
                 "\xff",
             }) {
            EXPECT_TRUE(tests::has_error(core::url::make(input), std::errc::invalid_argument)) << input;
        }

        // Once a domain is non-ASCII, IDNA validates its Punycode labels too, while opaque hosts are just
        // percent-encoded.
        EXPECT_TRUE(tests::has_error(core::url::make("http://xn--a.\xC3\x9F/"), std::errc::invalid_argument));

        auto opaque = core::url::make("sc://m\xC3\xBCnchen.de/");
        ASSERT_TRUE(opaque) << opaque.error();
        EXPECT_EQ(opaque->host(), "m%C3%BCnchen.de");
    }

    TEST(url, parts_are_normalized_and_validated) {
        auto built = core::url::make({
            .scheme = "HTTPS",
            .host = "Example.com",
            .port = 443,
            .path = "a b",
            .query = "x y",
            .fragment = "z",
        });
        ASSERT_TRUE(built) << built.error();
        EXPECT_EQ(built->serialize(), "https://example.com/a%20b?x%20y#z");

        // Without a host, a path that doesn't start with "/" is opaque, and "?" can't end it early.
        auto mail = core::url::make({.scheme = "mailto", .path = "me@example.com?subject"});
        ASSERT_TRUE(mail) << mail.error();
        EXPECT_EQ(mail->host(), std::nullopt);
        EXPECT_EQ(mail->serialize(), "mailto:me@example.com%3Fsubject");

        EXPECT_TRUE(tests::has_error(core::url::make({.scheme = "https"}), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(core::url::make({.scheme = "1x", .host = "a"}), std::errc::invalid_argument));
        EXPECT_TRUE(
            tests::has_error(core::url::make({.scheme = "https", .host = "a/b"}), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(core::url::make({.scheme = "file", .username = "me", .host = ""}),
                              std::errc::invalid_argument));
    }

    TEST(url, writers_normalize_or_leave_the_url_unchanged) {
        auto link = core::url::make("http://example.com:8080/a?q#f");
        ASSERT_TRUE(link) << link.error();

        EXPECT_TRUE(link->scheme("https"));
        EXPECT_TRUE(link->port(443));
        EXPECT_EQ(link->port(), std::nullopt);
        EXPECT_TRUE(link->host("EXAMPLE.org"));
        EXPECT_TRUE(link->username("me"));
        EXPECT_TRUE(link->password("p:w"));
        EXPECT_TRUE(link->path("/x/../y z"));
        EXPECT_TRUE(link->query(std::nullopt));
        EXPECT_TRUE(link->fragment("a b"));
        EXPECT_EQ(link->serialize(), "https://me:p%3Aw@example.org/y%20z#a%20b");

        auto before = link->serialize();
        EXPECT_TRUE(tests::has_error(link->scheme("foo"), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(link->host(""), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(link->host("example.org/path"), std::errc::invalid_argument));
        EXPECT_EQ(link->serialize(), before);

        auto mail = core::url::make("mailto:me@example.com");
        ASSERT_TRUE(mail) << mail.error();
        EXPECT_TRUE(tests::has_error(mail->host("example.com"), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(mail->path("/other"), std::errc::invalid_argument));

        auto file = core::url::make("file:///tmp");
        ASSERT_TRUE(file) << file.error();
        EXPECT_TRUE(tests::has_error(file->port(8080), std::errc::invalid_argument));
        EXPECT_TRUE(tests::has_error(file->username("me"), std::errc::invalid_argument));
    }

    TEST(url, urls_own_their_components_while_views_borrow_them) {
        const std::string text = "https://user@example.com:8443/p?q#f";

        auto owned = core::url::make(text);
        ASSERT_TRUE(owned) << owned.error();

        auto copy = *owned;
        ASSERT_TRUE(copy.path("/other"));
        EXPECT_EQ(owned->path(), "/p");

        core::url_view borrowed = *owned;
        EXPECT_EQ(borrowed.path().data(), owned->path().data());

        auto view = core::url_view::make(text);
        ASSERT_TRUE(view) << view.error();
        EXPECT_EQ(view->host()->data(), text.data() + text.find("example.com"));
        EXPECT_EQ(view->path().data(), text.data() + text.find("/p"));
        EXPECT_EQ(view->serialize(), text);

        auto shared = *view;
        EXPECT_EQ(shared.path().data(), view->path().data());

        core::url adopted{*view};
        EXPECT_NE(adopted.path().data(), view->path().data());
        EXPECT_EQ(adopted, *owned);

        // Anything the parser would have to rewrite can't be viewed.
        for (std::string_view input :
             {"HTTPS://example.com/", "https://example.com", "https://example.com:443/"}) {
            EXPECT_TRUE(tests::has_error(core::url_view::make(input), std::errc::invalid_argument)) << input;
            EXPECT_TRUE(core::url::make(input)) << input;
        }
    }

    TEST(url, file_paths_percent_encode_what_the_parser_would_interpret) {
        auto file = core::url::from_path("/tmp/a b/#1%?.txt");
        ASSERT_TRUE(file) << file.error();
        EXPECT_EQ(file->serialize(), "file:///tmp/a%20b/%231%25%3F.txt");

        EXPECT_TRUE(tests::has_error(core::url::from_path("relative/path"), std::errc::invalid_argument));
    }
} // namespace rei::core::tests
