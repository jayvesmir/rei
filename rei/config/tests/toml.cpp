#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"

#include "rei/config/config.hpp"
#include "rei/config/error_code.hpp"
#include "rei/config/tests/support.hpp"
#include "rei/core/error.hpp"
#include "rei/formats/error_code.hpp"

namespace rei::config::tests {
    namespace {
        enum class level : std::uint8_t { trace, debug, info, warn, error };

        struct settings {
            std::string host{"0.0.0.0"};
            std::uint16_t port{8080};
            std::vector<std::string> index_files{"index.html"};
            std::chrono::seconds io_timeout{60};
            level log_level{level::info};
        };

        auto declare(config::command& app, settings& bound) -> void {
            app.option("http.host", bound.host);
            app.option("http.port", bound.port, {.short_name = 'p'});
            app.option("http.index_files", bound.index_files);
            app.option("http.io_timeout", bound.io_timeout);
            app.option("log.level", bound.log_level);
        }
    } // namespace

    TEST(config_toml, documents_flatten_into_layers) {
        const auto layer = config::parse_toml(R"(title = "site"
limits.max_body_size = 1_048_576

[http]
port = 0x20FB
ratio = 1e3
verbose = true
since = 1979-05-27 07:32:00Z
index_files = [
    "index.html",
    "index.htm",
]
tls = { min_version = "1.3" }

[log]
level = "warn"
)",
                                              "site.toml");
        ASSERT_TRUE(layer) << layer.error();
        EXPECT_EQ(tests::describe(*layer), (std::vector<std::string>{
                                               "title=site (site.toml:1)",
                                               "limits.max_body_size=1048576 (site.toml:2)",
                                               "http.port=8443 (site.toml:5)",
                                               "http.ratio=1000.0 (site.toml:6)",
                                               "http.verbose=true (site.toml:7)",
                                               "http.since=1979-05-27T07:32:00Z (site.toml:8)",
                                               "http.index_files=index.html (site.toml:10)",
                                               "http.index_files=index.htm (site.toml:11)",
                                               "http.tls.min_version=1.3 (site.toml:13)",
                                               "log.level=warn (site.toml:16)",
                                           }));

        const auto unnamed = config::parse_toml("[\"\"]\nport = 2\n", "site.toml");
        ASSERT_TRUE(unnamed) << unnamed.error();
        EXPECT_EQ(tests::describe(*unnamed), (std::vector<std::string>{".port=2 (site.toml:2)"}));

        settings bound;
        std::vector<rei::error> warnings;
        config::command app{
            "app", {.on_warning = [&](const rei::error& warning) -> void { warnings.push_back(warning); }}};
        declare(app, bound);

        const auto use = [&](std::string_view text,
                             std::string_view source = "site.toml") -> std::expected<void, rei::error> {
            return config::parse_toml(text, source)
                .and_then([&](const config::layer& parsed) -> std::expected<void, rei::error> {
                    return app.apply(std::array{parsed});
                });
        };

        const auto used = use("[http]\nport = \"8443\"\nio_timeout = \"30s\"\n\n[htpp]\nhost = \"::1\"\n");
        ASSERT_TRUE(used) << used.error();
        EXPECT_EQ(bound.port, 8443);
        EXPECT_EQ(bound.io_timeout, std::chrono::seconds{30});
        EXPECT_EQ(bound.host, "0.0.0.0");

        ASSERT_EQ(warnings.size(), 1u);
        EXPECT_EQ(warnings[0].detail(), "site.toml:6: unknown key 'htpp.host'; did you mean 'http.host'?");

        EXPECT_TRUE(tests::fails_with(use("[http]\nport = 70000\n"), config::error_code::invalid_value,
                                      "site.toml:2: http.port: 70000 is out of range (0 to 65535)"));
        EXPECT_TRUE(tests::fails_with(use("[http]\nport = 70000\n", {}), config::error_code::invalid_value,
                                      "2: http.port: 70000 is out of range (0 to 65535)"));
        EXPECT_TRUE(tests::fails_with(use("[http]\nio_timeout = 30\n"), config::error_code::invalid_value,
                                      "site.toml:2: http.io_timeout: '30' is not a duration such as 1m30s"));
        EXPECT_TRUE(tests::fails_with(
            use("[log]\nlevel = [\n  \"warn\",\n  \"loud\",\n]\n"), config::error_code::invalid_value,
            "site.toml:4: log.level: 'loud' is not one of trace, debug, info, warn, "
            "error"));

        EXPECT_TRUE(tests::fails_with(config::parse_toml("[http]\nport 80\n", "site.toml"),
                                      formats::error_code::invalid_syntax,
                                      "site.toml:2:6: expected '=' after a key"));
        EXPECT_TRUE(tests::fails_with(config::parse_toml("port 80\n"), formats::error_code::invalid_syntax,
                                      "1:6: expected '=' after a key"));

        EXPECT_TRUE(tests::fails_with(config::parse_toml("[[mirrors]]\nport = 1\n", "site.toml"),
                                      config::error_code::unsupported_value,
                                      "site.toml:1: mirrors: an array of tables is not supported"));
        EXPECT_TRUE(tests::fails_with(config::parse_toml("[http]\ngrid = [[1, 2], [3]]\n", "site.toml"),
                                      config::error_code::unsupported_value,
                                      "site.toml:2: http.grid: an array of arrays is not supported"));
        EXPECT_TRUE(tests::fails_with(config::parse_toml("[http]\nindex_files = []\n", "site.toml"),
                                      config::error_code::unsupported_value,
                                      "site.toml:2: http.index_files: an empty array is not supported"));
    }

    TEST(config_toml, load_reads_configuration_files) {
        settings bound;
        std::optional<std::filesystem::path> config;

        config::command app{"app", {.env_prefix = "REI_CONFIG_FILES"}};
        app.option("config", config, {.short_name = 'c', .config_file = true});
        declare(app, bound);

        const std::array<const char*, 3> argv{"app", "-p", "443"};
        const auto loaded = app.load(static_cast<int>(argv.size()), argv.data());
        ASSERT_TRUE(loaded) << loaded.error();
        ASSERT_TRUE(config);
        EXPECT_EQ(config->filename(), "site.toml");
        EXPECT_EQ(bound.host, "::1");
        EXPECT_EQ(bound.port, 443);
        EXPECT_EQ(bound.index_files, (std::vector<std::string>{"index.html", "index.htm"}));
        EXPECT_EQ(bound.io_timeout, std::chrono::seconds{30});
        EXPECT_EQ(bound.log_level, level::warn);

        const auto site = config->string();
        const auto overrides = (config->parent_path() / "override.toml").string();

        const std::array<const char*, 3> none{"app", "--config", "none"};
        ASSERT_TRUE(app.load(static_cast<int>(none.size()), none.data()));
        EXPECT_FALSE(config);

        settings layered;
        std::vector<std::filesystem::path> configs;
        config::command several{"app"};
        several.option("config", configs, {.short_name = 'c', .config_file = true});
        declare(several, layered);

        const std::array<const char*, 5> both{"app", "-c", site.c_str(), "-c", overrides.c_str()};
        const auto merged = several.load(static_cast<int>(both.size()), both.data());
        ASSERT_TRUE(merged) << merged.error();
        EXPECT_EQ(layered.host, "127.0.0.1");
        EXPECT_EQ(layered.port, 9000);
        EXPECT_EQ(layered.index_files, (std::vector<std::string>{"home.html"}));
        EXPECT_EQ(layered.log_level, level::warn);
    }

    TEST(config_toml, only_a_missing_default_file_is_skipped) {
        const std::array<const char*, 1> bare{"app"};
        const std::array<const char*, 3> absent{"app", "--config", "missing.toml"};

        settings untouched;
        std::filesystem::path fallback{"missing.toml"};
        config::command quiet{"app"};
        quiet.option("config", fallback, {.config_file = true});
        declare(quiet, untouched);

        const auto skipped = quiet.load(static_cast<int>(bare.size()), bare.data());
        ASSERT_TRUE(skipped) << skipped.error();
        EXPECT_EQ(untouched.port, 8080);

        const auto failed = quiet.load(static_cast<int>(absent.size()), absent.data());
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error().code(), std::errc::no_such_file_or_directory);
        EXPECT_EQ(failed.error().detail(), std::format("missing.toml: {}", failed.error().code().message()));

        std::filesystem::path named;
        config::command strict{"app", {.env_prefix = "REI_CONFIG_MISSING"}};
        strict.option("config", named, {.config_file = true});

        const auto refused = strict.load(static_cast<int>(bare.size()), bare.data());
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code(), std::errc::no_such_file_or_directory);

        std::filesystem::path directory{"."};
        config::command unreadable{"app"};
        unreadable.option("config", directory, {.config_file = true});

        const auto denied = unreadable.load(static_cast<int>(bare.size()), bare.data());
        ASSERT_FALSE(denied);
        EXPECT_NE(denied.error().code(), std::errc::no_such_file_or_directory);
    }
} // namespace rei::config::tests
