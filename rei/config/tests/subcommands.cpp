#include <array>
#include <cstdint>
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
        struct tool {
            bool verbose{};
            std::uint16_t port{8080};
            std::string root{"."};
            std::uint16_t mirror_port{9090};
            std::string name;
            bool force{};
            std::vector<rei::error> warnings;

            config::command app{
                "app",
                {
                    .help = "Serves and mirrors directories",
                    .version = "1.2.0",
                    .env_prefix = "APP",
                    .on_warning = [this](const rei::error& warning) -> void { warnings.push_back(warning); },
                }};
            config::command& serve = app.subcommand("serve", "Serve a directory");
            config::command& remote = app.subcommand("remote", "Manage remotes");
            config::command& add = remote.subcommand("add", "Add a remote");
            config::command& remove = remote.subcommand("remove", "Remove a remote");

            tool() {
                app.option("log.verbose", verbose, {.short_name = 'v', .long_name = "verbose"});
                serve.option("http.port", port, {.short_name = 'p'});
                serve.argument("root", root);
                add.option("http.port", mirror_port, {.help = "Port of the remote", .env = "MIRROR_PORT"});
                add.argument("name", name, {.help = "Name of the remote", .required = true});
                remove.option("force", force, {.short_name = 'f'});
            }
        };
    } // namespace

    TEST(config_subcommands, arguments_select_a_path) {
        tool tool;

        const auto served = tests::read(tool.app, {"-v", "serve", "-p", "80", "site"});
        ASSERT_TRUE(served) << served.error();
        EXPECT_EQ(tests::describe(*served), (std::vector<std::string>{
                                                "log.verbose=true (-v)",
                                                "http.port=80 (-p)",
                                                "root=site (<ROOT>)",
                                            }));
        EXPECT_TRUE(tool.app.selected());
        EXPECT_TRUE(tool.serve.selected());
        EXPECT_FALSE(tool.remote.selected());

        const auto added = tests::read(tool.app, {"remote", "add", "--http.port=81", "origin"});
        ASSERT_TRUE(added) << added.error();
        EXPECT_EQ(tests::describe(*added),
                  (std::vector<std::string>{"http.port=81 (--http.port)", "name=origin (<NAME>)"}));
        EXPECT_FALSE(tool.serve.selected());
        EXPECT_TRUE(tool.remote.selected());
        EXPECT_TRUE(tool.add.selected());
        EXPECT_FALSE(tool.remove.selected());

        const auto ended = tests::read(tool.app, {"--", "serve", "-p"});
        ASSERT_TRUE(ended) << ended.error();
        EXPECT_EQ(tests::describe(*ended), (std::vector<std::string>{"root=-p (<ROOT>)"}));

        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"serve", "-v"}),
                                      config::error_code::unknown_option, "unknown option '-v'"));
        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"-p", "80", "serve"}),
                                      config::error_code::unknown_option, "unknown option '-p'"));
        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"serve", "--version"}),
                                      config::error_code::unknown_option, "unknown option '--version'"));
        EXPECT_EQ(tests::read(tool.app, {"serve", "--help"}).error().code(),
                  config::error_code::help_requested);

        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"serv"}), config::error_code::unknown_command,
                                      "unknown command 'serv'; did you mean 'serve'?"));
        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"remote", "list"}),
                                      config::error_code::unknown_command, "unknown command 'list'"));
        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"-v"}), config::error_code::missing_command,
                                      "missing command"));
        EXPECT_TRUE(tests::fails_with(tests::read(tool.app, {"remote"}), config::error_code::missing_command,
                                      "missing command"));
    }

    TEST(config_subcommands, only_the_selected_path_is_applied) {
        tool tool;

        const std::array<std::string_view, 5> environment{
            "APP__LOG__VERBOSE=true", "APP__HTTP__PORT=81", "MIRROR_PORT=82",
            "APP__FORCE=true",        "APP__HTTP__PROT=83",
        };

        const auto args = tests::read(tool.app, {"remote", "add", "origin"});
        ASSERT_TRUE(args) << args.error();

        const auto env = tool.app.read_env(environment);
        ASSERT_TRUE(env) << env.error();
        EXPECT_EQ(tests::describe(*env), (std::vector<std::string>{
                                             "log.verbose=true (APP__LOG__VERBOSE)",
                                             "http.port=82 (MIRROR_PORT)",
                                             "force=true (APP__FORCE)",
                                             "http.prot=83 (APP__HTTP__PROT)",
                                         }));

        const auto applied = tool.app.apply(std::array{*env, *args});
        ASSERT_TRUE(applied) << applied.error();
        EXPECT_TRUE(tool.verbose);
        EXPECT_EQ(tool.mirror_port, 82);
        EXPECT_EQ(tool.name, "origin");
        EXPECT_EQ(tool.port, 8080);
        EXPECT_FALSE(tool.force);

        ASSERT_EQ(tool.warnings.size(), 1u);
        EXPECT_EQ(tool.warnings[0].detail(),
                  "APP__HTTP__PROT: unknown key 'http.prot'; did you mean 'http.port'?");

        ASSERT_TRUE(tests::read(tool.app, {"serve"}));

        const auto served = tool.app.read_env(environment);
        ASSERT_TRUE(served) << served.error();
        EXPECT_EQ(tests::describe(*served), (std::vector<std::string>{
                                                "log.verbose=true (APP__LOG__VERBOSE)",
                                                "http.port=81 (APP__HTTP__PORT)",
                                                "force=true (APP__FORCE)",
                                                "http.prot=83 (APP__HTTP__PROT)",
                                            }));

        ASSERT_TRUE(tool.app.apply(std::array{*served}));
        EXPECT_EQ(tool.port, 81);
        EXPECT_EQ(tool.mirror_port, 82);

        EXPECT_TRUE(tests::fails_with(tests::load(tool.app, {"remote", "add"}),
                                      config::error_code::missing_required,
                                      "missing required argument <NAME>"));
    }

    TEST(config_subcommands, help_and_report_follow_the_selection) {
        tool tool;

        EXPECT_EQ(tool.app.help(), R"(Serves and mirrors directories

Usage: app [OPTIONS] <serve|remote>

Commands:
  serve   Serve a directory
  remote  Manage remotes

Options:
  -v, --verbose  [env: APP__LOG__VERBOSE]
  -h, --help     Print help
  -V, --version  Print version
)");

        EXPECT_EQ(tool.add.help(), R"(Add a remote

Usage: app remote add [OPTIONS] <NAME>

Arguments:
  <NAME>  Name of the remote [env: APP__NAME]

Options:
      --http.port <PORT>  Port of the remote [default: 9090] [env: MIRROR_PORT]
  -h, --help              Print help
)");

        const auto asked = tests::read(tool.app, {"remote", "add", "--help"});
        ASSERT_FALSE(asked);

        testing::internal::CaptureStdout();
        EXPECT_EQ(tool.app.report(asked.error()), 0);
        EXPECT_EQ(testing::internal::GetCapturedStdout(), tool.add.help());

        const auto missing = tests::read(tool.app, {"remote"});
        ASSERT_FALSE(missing);

        testing::internal::CaptureStderr();
        EXPECT_EQ(tool.app.report(missing.error()), 2);
        EXPECT_EQ(testing::internal::GetCapturedStderr(), "error: missing command\n"
                                                          "\n"
                                                          "Usage: app remote [OPTIONS] <add|remove>\n"
                                                          "\n"
                                                          "For more information, try '--help'.\n");
    }
} // namespace rei::config::tests
