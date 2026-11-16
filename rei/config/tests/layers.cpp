#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "rei/config/config.hpp"
#include "rei/config/error_code.hpp"
#include "rei/config/tests/support.hpp"
#include "rei/core/error.hpp"

namespace rei::config {
    namespace tests {
        namespace {
            enum class level : std::uint8_t { trace, debug, info, warn, error };

            struct endpoint {
                std::string host;
                std::uint16_t port{};
            };
        } // namespace
    } // namespace tests

    template <> struct parser<tests::endpoint> {
        static auto parse(std::string_view text) -> std::expected<tests::endpoint, std::string> {
            const auto colon = text.rfind(':');
            const auto port = parser<std::uint16_t>::parse(text.substr(colon + 1));

            if (colon == std::string_view::npos || !port) {
                return std::unexpected(std::string{"expected HOST:PORT"});
            }

            return tests::endpoint{.host = std::string{text.substr(0, colon)}, .port = *port};
        }
    };
} // namespace rei::config

namespace rei::config::tests {
    TEST(config_values, conversions) {
        std::uint16_t port{};
        std::int8_t niceness{};
        double ratio{};
        bool verbose{};
        std::chrono::seconds timeout{};
        std::optional<std::chrono::milliseconds> io_timeout{std::chrono::seconds{60}};
        std::optional<std::uint64_t> limit{1024};
        level log_level{level::info};
        std::filesystem::path root;
        endpoint listen;

        config::command app{"app"};
        app.option("port", port);
        app.option("niceness", niceness);
        app.option("ratio", ratio);
        app.option("verbose", verbose);
        app.option("timeout", timeout);
        app.option("io_timeout", io_timeout);
        app.option("limit", limit);
        app.option("level", log_level);
        app.option("root", root);
        app.option("listen", listen);

        const auto invalid = [&](std::string_view key, std::string_view text,
                                 std::string_view reason) -> testing::AssertionResult {
            return tests::fails_with(tests::set(app, key, text), config::error_code::invalid_value,
                                     std::format("{}: {}", key, reason));
        };

        ASSERT_TRUE(tests::set(app, "port", "65535"));
        EXPECT_EQ(port, 65535);
        EXPECT_TRUE(invalid("port", "65536", "65536 is out of range (0 to 65535)"));
        EXPECT_TRUE(invalid("port", "-1", "-1 is out of range (0 to 65535)"));
        EXPECT_TRUE(invalid("port", "80x", "'80x' is not a number"));
        EXPECT_TRUE(invalid("port", "", "'' is not a number"));

        ASSERT_TRUE(tests::set(app, "niceness", "-128"));
        EXPECT_EQ(niceness, -128);
        EXPECT_TRUE(invalid("niceness", "128", "128 is out of range (-128 to 127)"));

        ASSERT_TRUE(tests::set(app, "ratio", "2.5e-1"));
        EXPECT_EQ(ratio, 0.25);
        EXPECT_TRUE(invalid("ratio", "half", "'half' is not a number"));
        EXPECT_TRUE(invalid("ratio", "1e999", "1e999 is out of range"));

        ASSERT_TRUE(tests::set(app, "verbose", "YES"));
        EXPECT_TRUE(verbose);
        ASSERT_TRUE(tests::set(app, "verbose", "off"));
        EXPECT_FALSE(verbose);
        EXPECT_TRUE(invalid("verbose", "2", "'2' is not a boolean"));

        ASSERT_TRUE(tests::set(app, "timeout", "1m30s"));
        EXPECT_EQ(timeout, std::chrono::seconds{90});
        ASSERT_TRUE(tests::set(app, "timeout", "2000ms"));
        EXPECT_EQ(timeout, std::chrono::seconds{2});
        EXPECT_TRUE(invalid("timeout", "1500ms", "1500ms is not a multiple of 1s"));
        EXPECT_TRUE(invalid("timeout", "90", "'90' is not a duration such as 1m30s"));
        EXPECT_TRUE(invalid("timeout", "1w", "'1w' is not a duration such as 1m30s"));
        EXPECT_TRUE(invalid("timeout", "999999999999999999d",
                            "999999999999999999d is out of range (up to 9223372036854775807s)"));
        EXPECT_TRUE(invalid("timeout", "9223372036854775807s1s",
                            "9223372036854775807s1s is out of range (up to 9223372036854775807s)"));

        ASSERT_TRUE(tests::set(app, "io_timeout", "1h"));
        EXPECT_EQ(io_timeout, std::chrono::hours{1});
        ASSERT_TRUE(tests::set(app, "io_timeout", "none"));
        EXPECT_FALSE(io_timeout);

        ASSERT_TRUE(tests::set(app, "limit", "none"));
        EXPECT_FALSE(limit);
        ASSERT_TRUE(tests::set(app, "limit", "42"));
        EXPECT_EQ(limit, 42u);
        EXPECT_TRUE(invalid("limit", "lots", "'lots' is not a number"));

        ASSERT_TRUE(tests::set(app, "level", "warn"));
        EXPECT_EQ(log_level, level::warn);
        EXPECT_TRUE(invalid("level", "warning", "'warning' is not one of trace, debug, info, warn, error"));

        ASSERT_TRUE(tests::set(app, "root", "/srv/www"));
        EXPECT_EQ(root, "/srv/www");
        ASSERT_TRUE(tests::set(app, "root", "/srv/caf\u00e9"));
        EXPECT_EQ(root, std::filesystem::path{u8"/srv/caf\u00e9"});
        EXPECT_TRUE(app.help().contains("[default: /srv/caf\u00e9]"));

        ASSERT_TRUE(tests::set(app, "listen", "localhost:80"));
        EXPECT_EQ(listen.host, "localhost");
        EXPECT_EQ(listen.port, 80);
        EXPECT_TRUE(invalid("listen", "localhost", "expected HOST:PORT"));
    }

    TEST(config_layers, highest_layer_with_the_key_wins) {
        std::uint16_t port{8080};
        std::string host{"0.0.0.0"};
        std::vector<std::string> index_files{"index.html"};
        std::vector<std::uint16_t> mirrors;
        level log_level{level::info};

        config::command app{"app"};
        app.option("http.port", port);
        app.option("http.host", host);
        app.option("http.index_files", index_files);
        app.option("http.mirrors", mirrors);
        app.option("log.level", log_level);

        const std::vector<config::layer> layers{
            {
                {.key = "http.port", .text = "lots", .origin = {.source = "site.toml", .line = 2}},
                {.key = "http.index_files", .text = "a.html", .origin = {.source = "site.toml", .line = 3}},
                {.key = "http.index_files", .text = "b.html", .origin = {.source = "site.toml", .line = 3}},
                {.key = "log.level", .text = "warn", .origin = {.source = "site.toml", .line = 6}},
            },
            {
                {.key = "http.port", .text = "8443", .origin = {.source = "APP__HTTP__PORT"}},
                {.key = "http.index_files", .text = "c.html", .origin = {.source = "APP__HTTP__INDEX_FILES"}},
            },
            {
                {.key = "http.port", .text = "1", .origin = {.source = "--http.port"}},
                {.key = "http.port", .text = "2", .origin = {.source = "-p"}},
            },
        };

        const auto applied = app.apply(layers);
        ASSERT_TRUE(applied) << applied.error();
        EXPECT_EQ(port, 2);
        EXPECT_EQ(host, "0.0.0.0");
        EXPECT_EQ(index_files, (std::vector<std::string>{"c.html"}));
        EXPECT_EQ(log_level, level::warn);

        EXPECT_TRUE(tests::fails_with(app.apply(std::span{layers}.first(1)),
                                      config::error_code::invalid_value,
                                      "site.toml:2: http.port: 'lots' is not a number"));
        EXPECT_EQ(index_files, (std::vector<std::string>{"c.html"}));

        const std::vector<config::layer> rejected{
            {
                {.key = "http.mirrors", .text = "8081", .origin = {.source = "site.toml", .line = 4}},
                {.key = "http.mirrors", .text = "70000", .origin = {.source = "site.toml", .line = 5}},
                {.key = "http.mirrors", .text = "8083", .origin = {.source = "site.toml", .line = 6}},
            },
            {{.key = "log.level", .text = "loud", .origin = {.source = "APP__LOG__LEVEL"}}},
        };
        EXPECT_TRUE(tests::fails_with(app.apply(std::span{rejected}.first(1)),
                                      config::error_code::invalid_value,
                                      "site.toml:5: http.mirrors: 70000 is out of range (0 to 65535)"));
        EXPECT_TRUE(mirrors.empty());
        EXPECT_TRUE(
            tests::fails_with(app.apply(std::span{rejected}.last(1)), config::error_code::invalid_value,
                              "APP__LOG__LEVEL: 'loud' is not one of trace, debug, info, warn, error"));
    }

    TEST(config_layers, required_options_and_unknown_keys) {
        std::string token;
        std::uint16_t port{8080};
        std::string root;
        std::vector<rei::error> warnings;

        config::command app{
            "app", {.on_warning = [&](const rei::error& warning) -> void { warnings.push_back(warning); }}};
        app.option("auth.token", token, {.required = true});
        app.option("http.port", port);
        app.argument("root", root, {.required = true});

        EXPECT_TRUE(tests::fails_with(tests::load(app, {"site"}), config::error_code::missing_required,
                                      "missing required option '--auth.token'"));
        EXPECT_TRUE(tests::fails_with(tests::load(app, {"--auth.token", "secret"}),
                                      config::error_code::missing_required,
                                      "missing required argument <ROOT>"));

        const std::vector<config::layer> layers{
            {
                {.key = "auth.token", .text = "secret", .origin = {.source = "site.toml", .line = 1}},
                {.key = "htpp.port", .text = "80", .origin = {.source = "site.toml", .line = 2}},
                {.key = "htpp.port", .text = "81", .origin = {.source = "site.toml", .line = 3}},
                {.key = "plugins.cache", .text = "64", .origin = {.source = "APP__PLUGINS__CACHE"}},
            },
            {{.key = "root", .text = "site", .origin = {.source = "<ROOT>"}}},
        };

        const auto applied = app.apply(layers);
        ASSERT_TRUE(applied) << applied.error();
        EXPECT_EQ(token, "secret");
        EXPECT_EQ(port, 8080);
        EXPECT_EQ(root, "site");

        ASSERT_EQ(warnings.size(), 2u);
        EXPECT_EQ(warnings[0].code(), config::error_code::unknown_key);
        EXPECT_EQ(warnings[0].detail(), "site.toml:2: unknown key 'htpp.port'; did you mean 'http.port'?");
        EXPECT_EQ(warnings[1].code(), config::error_code::unknown_key);
        EXPECT_EQ(warnings[1].detail(), "APP__PLUGINS__CACHE: unknown key 'plugins.cache'");
    }

    TEST(config_schema, validation) {
        bool flag{};
        std::string text;
        std::vector<std::string> list;
        std::filesystem::path path;
        std::vector<std::filesystem::path> paths;

        const auto problem = [](const auto& declare) -> std::string {
            config::command app{"app", {.env_prefix = "APP"}};
            declare(app);

            const auto applied = app.apply({});
            const auto read = tests::read(app, {"--help"});
            const auto env = app.read_env({});
            if (applied || env || read.error() != applied.error() || env.error() != applied.error()) {
                return "accepted";
            }

            return applied.error().detail();
        };

        for (const std::string_view key : {"a", "x1.y_2", "http.max_body_size"}) {
            EXPECT_EQ(problem([&](config::command& app) -> void { app.option(key, text); }), "accepted")
                << key;
        }
        for (const std::string_view key :
             {"", "Http.Port", "http..port", "http.max__size", "http.port_", "9lives", "dry-run"}) {
            EXPECT_EQ(problem([&](config::command& app) -> void { app.option(key, text); }),
                      std::format("invalid key '{}'", key));
        }

        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("root", text);
                      app.argument("root", text);
                  }),
                  "duplicate key 'root'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("dry_run", flag, {.long_name = "dry-run"});
                  }),
                  "invalid option name '--dry-run'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("verbose", flag);
                      app.option("log.verbose", flag, {.long_name = "verbose"});
                  }),
                  "duplicate option name '--verbose'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("http.port", text, {.short_name = 'p'});
                      app.option("http.path", text, {.short_name = 'p'});
                  }),
                  "duplicate option name '-p'");
        EXPECT_EQ(
            problem([&](config::command& app) -> void { app.option("stdin", flag, {.short_name = '-'}); }),
            "invalid short name for 'stdin'");
        EXPECT_EQ(
            problem([&](config::command& app) -> void { app.argument("root", text, {.short_name = 'r'}); }),
            "argument 'root' has an option name");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("http.port", text, {.env = "PORT"});
                      app.argument("port", text, {.env = "PORT"});
                  }),
                  "duplicate environment name 'PORT'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("http.port", text, {.env = "APP__PORT"});
                  }),
                  "environment name 'APP__PORT' starts with 'APP__'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("dry_run", flag, {.env = ""});
                      app.option("force", flag, {.env = ""});
                  }),
                  "accepted");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.argument("files", list);
                      app.argument("root", text);
                  }),
                  "argument 'root' follows a variadic one");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.argument("source", text);
                      app.argument("target", text, {.required = true});
                  }),
                  "required argument 'target' follows an optional one");

        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.subcommand("serve").option("http.port", text, {.env = "PORT"});
                      app.subcommand("check").option("http.port", text, {.env = "PORT"});
                  }),
                  "accepted");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("http.port", text);
                      app.subcommand("remote").subcommand("add").option("http.port", text);
                  }),
                  "duplicate key 'http.port'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("http.port", text, {.env = "PORT"});
                      app.subcommand("serve").option("health.port", text, {.env = "PORT"});
                  }),
                  "duplicate environment name 'PORT'");
        EXPECT_EQ(problem([&](config::command& app) -> void { app.subcommand("dry-run"); }),
                  "invalid command name 'dry-run'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.subcommand("serve");
                      app.subcommand("serve");
                  }),
                  "duplicate command 'serve'");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.argument("root", text);
                      app.subcommand("serve");
                  }),
                  "argument 'root' is on a command with subcommands");

        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.subcommand("serve").option("config", path, {.config_file = true});
                      app.subcommand("check").option("config", paths, {.config_file = true});
                  }),
                  "accepted");
        EXPECT_EQ(
            problem([&](config::command& app) -> void { app.option("config", text, {.config_file = true}); }),
            "configuration file option 'config' is not a path");
        EXPECT_EQ(problem([&](config::command& app) -> void {
                      app.option("config", path, {.config_file = true});
                      app.subcommand("serve").option("overrides", paths, {.config_file = true});
                  }),
                  "'overrides' is a second configuration file option");
    }
} // namespace rei::config::tests
