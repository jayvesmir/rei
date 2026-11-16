#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "rei/config/config.hpp"
#include "rei/config/error_code.hpp"
#include "rei/config/tests/support.hpp"

namespace rei::config::tests {
    namespace {
        struct settings {
            bool verbose{};
            bool color{true};
            std::uint16_t port{8080};
            std::string host{"0.0.0.0"};
            std::int32_t offset{};
            std::vector<std::string> index_files{"index.html"};
            std::string root{"."};
            std::vector<std::string> extra;
        };

        auto make_app(settings& bound, std::string_view version = {}) -> config::command {
            config::command app{"app", {.version = version}};

            app.option("log.verbose", bound.verbose, {.short_name = 'v', .long_name = "verbose"});
            app.option("log.color", bound.color);
            app.option("http.port", bound.port, {.short_name = 'p'});
            app.option("http.host", bound.host);
            app.option("offset", bound.offset, {.short_name = 'o'});
            app.option("http.index_files", bound.index_files);
            app.argument("root", bound.root);
            app.argument("extra", bound.extra);

            return app;
        }
    } // namespace

    TEST(config_args, grammar) {
        settings bound;
        auto app = make_app(bound);

        const std::vector<std::string_view> args{
            "-vp80",     "--http.host=::1",
            "site",      "--no-log.color",
            "--offset",  "-5",
            "-o7",       "--http.index_files",
            "a.html",    "--http.index_files=b.html",
            "-",         "--",
            "--verbose", "-x",
        };

        const auto layer = app.read_args(args);
        ASSERT_TRUE(layer) << layer.error();
        EXPECT_EQ(tests::describe(*layer), (std::vector<std::string>{
                                               "log.verbose=true (-v)",
                                               "http.port=80 (-p)",
                                               "http.host=::1 (--http.host)",
                                               "root=site (<ROOT>)",
                                               "log.color=false (--no-log.color)",
                                               "offset=-5 (--offset)",
                                               "offset=7 (-o)",
                                               "http.index_files=a.html (--http.index_files)",
                                               "http.index_files=b.html (--http.index_files)",
                                               "extra=- (<EXTRA>)",
                                               "extra=--verbose (<EXTRA>)",
                                               "extra=-x (<EXTRA>)",
                                           }));

        const auto applied = app.apply(std::array{*layer});
        ASSERT_TRUE(applied) << applied.error();
        EXPECT_TRUE(bound.verbose);
        EXPECT_FALSE(bound.color);
        EXPECT_EQ(bound.port, 80);
        EXPECT_EQ(bound.host, "::1");
        EXPECT_EQ(bound.offset, 7);
        EXPECT_EQ(bound.index_files, (std::vector<std::string>{"a.html", "b.html"}));
        EXPECT_EQ(bound.root, "site");
        EXPECT_EQ(bound.extra, (std::vector<std::string>{"-", "--verbose", "-x"}));

        const std::array<const char*, 4> argv{"app", "--verbose=off", "-p", "443"};
        const auto from_main = app.read_args(static_cast<int>(argv.size()), argv.data());
        ASSERT_TRUE(from_main) << from_main.error();
        EXPECT_EQ(tests::describe(*from_main),
                  (std::vector<std::string>{"log.verbose=off (--verbose)", "http.port=443 (-p)"}));
    }

    TEST(config_args, errors) {
        settings bound;
        auto app = make_app(bound);

        EXPECT_TRUE(tests::fails_with(tests::read(app, {"--http.prot", "80"}),
                                      config::error_code::unknown_option,
                                      "unknown option '--http.prot'; did you mean '--http.port'?"));
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"--no-verbos"}), config::error_code::unknown_option,
                                      "unknown option '--no-verbos'; did you mean '--no-verbose'?"));
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"--no-http.port"}),
                                      config::error_code::unknown_option, "unknown option '--no-http.port'"));
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"--log.verbose"}), config::error_code::unknown_option,
                                      "unknown option '--log.verbose'"));
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"-vx"}), config::error_code::unknown_option,
                                      "unknown option '-x'"));

        EXPECT_TRUE(tests::fails_with(tests::read(app, {"-v", "--http.port"}),
                                      config::error_code::missing_value,
                                      "option '--http.port' requires a value"));
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"-vp"}), config::error_code::missing_value,
                                      "option '-p' requires a value"));
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"--no-verbose=true"}),
                                      config::error_code::invalid_value,
                                      "option '--no-verbose' doesn't take a value"));

        settings fixed;
        config::command single{"app"};
        single.option("log.verbose", fixed.verbose, {.long_name = "verbose"});
        single.argument("root", fixed.root);
        EXPECT_TRUE(tests::fails_with(tests::read(single, {"--verbose", "false", "b"}),
                                      config::error_code::unexpected_argument, "unexpected argument 'b'"));

        EXPECT_TRUE(tests::fails_with(tests::load(app, {"--http.port", "--help"}),
                                      config::error_code::invalid_value,
                                      "--http.port: '--help' is not a number"));
        EXPECT_TRUE(tests::fails_with(tests::load(app, {"x", "--verbose=maybe"}),
                                      config::error_code::invalid_value,
                                      "--verbose: 'maybe' is not a boolean"));
    }

    TEST(config_args, help_and_version_are_error_codes) {
        settings bound;
        auto app = make_app(bound, "1.2.0");

        EXPECT_EQ(tests::read(app, {"-v", "--help", "--bogus"}).error().code(),
                  config::error_code::help_requested);
        EXPECT_EQ(tests::read(app, {"-vh"}).error().code(), config::error_code::help_requested);
        EXPECT_EQ(tests::read(app, {"--version"}).error().code(), config::error_code::version_requested);
        EXPECT_EQ(tests::read(app, {"-V"}).error().code(), config::error_code::version_requested);
        EXPECT_TRUE(tests::fails_with(tests::read(app, {"--hepl"}), config::error_code::unknown_option,
                                      "unknown option '--hepl'; did you mean '--help'?"));

        auto unversioned = make_app(bound);
        EXPECT_TRUE(tests::fails_with(tests::read(unversioned, {"--version"}),
                                      config::error_code::unknown_option, "unknown option '--version'"));

        std::string host;
        bool help{};
        config::command taken{"app"};
        taken.option("host", host, {.short_name = 'h'});
        taken.option("help", help);

        const auto layer = tests::read(taken, {"-h", "::1", "--help"});
        ASSERT_TRUE(layer) << layer.error();
        EXPECT_EQ(tests::describe(*layer), (std::vector<std::string>{"host=::1 (-h)", "help=true (--help)"}));
    }
} // namespace rei::config::tests
