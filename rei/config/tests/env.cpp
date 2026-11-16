#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "rei/config/config.hpp"
#include "rei/config/error_code.hpp"
#include "rei/config/tests/support.hpp"
#include "rei/core/error.hpp"

namespace rei::config::tests {
    namespace {
        struct settings {
            std::string host{"0.0.0.0"};
            std::uint16_t port{8080};
            std::vector<std::string> index_files{"index.html"};
            std::optional<std::uint64_t> max_body_size{1024};
            bool verbose{};
            bool dry_run{};
            std::string token;
            std::string root{"."};
        };

        auto declare(config::command& app, settings& bound) -> void {
            app.option("http.host", bound.host);
            app.option("http.port", bound.port, {.short_name = 'p', .env = "PORT"});
            app.option("http.index_files", bound.index_files);
            app.option("http.max_body_size", bound.max_body_size);
            app.option("log.verbose", bound.verbose, {.long_name = "verbose"});
            app.option("dry_run", bound.dry_run, {.env = ""});
            app.option("auth.token", bound.token, {.required = true});
            app.argument("root", bound.root);
        }
    } // namespace

    TEST(config_env, names_and_values) {
        settings bound;
        std::vector<rei::error> warnings;

        config::command app{
            "app",
            {
                .env_prefix = "APP",
                .on_warning = [&](const rei::error& warning) -> void { warnings.push_back(warning); },
            }};
        declare(app, bound);

        const std::array<std::string_view, 14> environment{
            "HOME=/root",
            "APP__HTTP__HOST=::1",
            "PORT=8443",
            "APP__HTTP__PORT=1",
            "APP__HTTP__INDEX_FILES=a.html,b.html",
            "APP__HTTP__MAX_BODY_SIZE=",
            "APP__LOG__VERBOSE=on",
            "APP__DRY_RUN=true",
            "APP__AUTH__TOKEN=a,b=c",
            "APP__HTTP__PROT=80",
            "APP__ROOT=site",
            "APP_ROOT=other",
            "APPLE=1",
            "APP__",
        };

        const auto layer = app.read_env(environment);
        ASSERT_TRUE(layer) << layer.error();
        EXPECT_EQ(tests::describe(*layer), (std::vector<std::string>{
                                               "http.host=::1 (APP__HTTP__HOST)",
                                               "http.port=8443 (PORT)",
                                               "http.index_files=a.html (APP__HTTP__INDEX_FILES)",
                                               "http.index_files=b.html (APP__HTTP__INDEX_FILES)",
                                               "log.verbose=on (APP__LOG__VERBOSE)",
                                               "auth.token=a,b=c (APP__AUTH__TOKEN)",
                                               "http.prot=80 (APP__HTTP__PROT)",
                                               "root=site (APP__ROOT)",
                                           }));

        const auto applied = app.apply(std::array{*layer});
        ASSERT_TRUE(applied) << applied.error();
        EXPECT_EQ(bound.host, "::1");
        EXPECT_EQ(bound.port, 8443);
        EXPECT_EQ(bound.index_files, (std::vector<std::string>{"a.html", "b.html"}));
        EXPECT_EQ(bound.max_body_size, 1024u);
        EXPECT_TRUE(bound.verbose);
        EXPECT_FALSE(bound.dry_run);
        EXPECT_EQ(bound.token, "a,b=c");
        EXPECT_EQ(bound.root, "site");

        ASSERT_EQ(warnings.size(), 1u);
        EXPECT_EQ(warnings[0].code(), config::error_code::unknown_key);
        EXPECT_EQ(warnings[0].detail(),
                  "APP__HTTP__PROT: unknown key 'http.prot'; did you mean 'http.port'?");

        const std::array<std::string_view, 2> invalid{
            "APP__AUTH__TOKEN=secret",
            "APP__HTTP__MAX_BODY_SIZE=lots",
        };
        const auto rejected = app.read_env(invalid);
        ASSERT_TRUE(rejected) << rejected.error();
        EXPECT_TRUE(tests::fails_with(app.apply(std::array{*rejected}), config::error_code::invalid_value,
                                      "APP__HTTP__MAX_BODY_SIZE: 'lots' is not a number"));

        settings plain;
        config::command unprefixed{"app"};
        declare(unprefixed, plain);

        const auto named = unprefixed.read_env(environment);
        ASSERT_TRUE(named) << named.error();
        EXPECT_EQ(tests::describe(*named), (std::vector<std::string>{"http.port=8443 (PORT)"}));
    }

    TEST(config_env, load_reads_args_and_the_process_environment) {
        settings bound;
        config::command app{"app", {.env_prefix = "REI_CONFIG_TESTS"}};
        app.option("http.host", bound.host);
        app.option("http.port", bound.port, {.short_name = 'p', .env = "REI_CONFIG_TESTS_PORT"});
        app.option("http.index_files", bound.index_files);
        app.option("auth.token", bound.token, {.required = true});
        app.argument("root", bound.root);

        const std::array<const char*, 5> argv{"app", "-p", "443", "--http.index_files", "c.html"};
        const auto loaded = app.load(static_cast<int>(argv.size()), argv.data());
        ASSERT_TRUE(loaded) << loaded.error();
        EXPECT_EQ(bound.host, "::1");
        EXPECT_EQ(bound.port, 443);
        EXPECT_EQ(bound.index_files, (std::vector<std::string>{"c.html"}));
        EXPECT_EQ(bound.token, "secret");
        EXPECT_EQ(bound.root, ".");

        const std::array<const char*, 2> help{"app", "--help"};
        const auto asked = app.load(static_cast<int>(help.size()), help.data());
        ASSERT_FALSE(asked);
        EXPECT_EQ(asked.error().code(), config::error_code::help_requested);

        const std::array<const char*, 1> bare{"app"};
        EXPECT_TRUE(tests::fails_with(app.load(static_cast<int>(bare.size()), bare.data()),
                                      config::error_code::invalid_value,
                                      "REI_CONFIG_TESTS_PORT: 'lots' is not a number"));
    }
} // namespace rei::config::tests
