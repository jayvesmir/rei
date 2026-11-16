#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
        enum class level : std::uint8_t { trace, debug, info, warn, error };

        struct settings {
            std::filesystem::path config{"/etc/fileserver.toml"};
            std::filesystem::path root{"."};
            std::vector<std::filesystem::path> mounts;
            std::string host{"0.0.0.0"};
            std::uint16_t port{8080};
            std::vector<std::string> index_files{"index.html", "index.htm"};
            std::optional<std::uint64_t> max_body_size{1024uz * 1024 * 1024};
            std::optional<std::chrono::milliseconds> io_timeout{std::chrono::seconds{60}};
            std::optional<std::size_t> max_connections;
            level log_level{level::info};
            bool verbose{};
            bool color{true};
            std::string secret{"hunter2"};
        };

        auto declare(config::command& app, settings& bound) -> void {
            app.option("config", bound.config, {.short_name = 'c', .help = "Configuration file"});
            app.option("http.host", bound.host, {.help = "Address to listen on"});
            app.option("http.port", bound.port,
                       {.short_name = 'p', .help = "Port to listen on", .env = "PORT"});
            app.option(
                "http.index_files", bound.index_files,
                {
                    .value_name = "FILE",
                    .help = "Files served for a directory, tried in the order given until one of them exists",
                });
            app.option("http.max_body_size", bound.max_body_size,
                       {.value_name = "BYTES", .help = "Largest request body, or none"});
            app.option("http.io_timeout", bound.io_timeout, {.help = "Read and write timeout, or none"});
            app.option("http.max_connections", bound.max_connections,
                       {.help = "Concurrent connections, or none"});
            app.option("log.level", bound.log_level, {.help = "Least severe level that is logged"});
            app.option("log.verbose", bound.verbose,
                       {.short_name = 'v', .long_name = "verbose", .help = "Log every request"});
            app.option("log.color", bound.color, {.help = "Color the log", .env = ""});
            app.option("debug.secret", bound.secret, {.hidden = true});
            app.argument("root", bound.root, {.help = "Directory to serve", .required = true});
            app.argument("mounts", bound.mounts,
                         {.value_name = "MOUNT", .help = "More directories to serve"});
        }
    } // namespace

    TEST(config_help, lists_arguments_and_options) {
        settings bound;
        config::command app{"fileserver",
                            {
                                .help = "Serves a directory over HTTP",
                                .version = "1.2.0",
                                .env_prefix = "FILESERVER",
                            }};
        declare(app, bound);

        EXPECT_EQ(app.help(), R"(Serves a directory over HTTP

Usage: fileserver [OPTIONS] <ROOT> [MOUNT]...

Arguments:
  <ROOT>      Directory to serve [env: FILESERVER__ROOT]
  [MOUNT]...  More directories to serve [env: FILESERVER__MOUNTS]

Options:
  -c, --config <CONFIG>               Configuration file [default: /etc/fileserver.toml]
                                      [env: FILESERVER__CONFIG]
      --http.host <HOST>              Address to listen on [default: 0.0.0.0]
                                      [env: FILESERVER__HTTP__HOST]
  -p, --http.port <PORT>              Port to listen on [default: 8080] [env: PORT]
      --http.index_files <FILE>       Files served for a directory, tried in the order given until
                                      one of them exists [default: index.html,index.htm]
                                      [env: FILESERVER__HTTP__INDEX_FILES]
      --http.max_body_size <BYTES>    Largest request body, or none [default: 1073741824]
                                      [env: FILESERVER__HTTP__MAX_BODY_SIZE]
      --http.io_timeout <IO_TIMEOUT>  Read and write timeout, or none [default: 60000ms]
                                      [env: FILESERVER__HTTP__IO_TIMEOUT]
      --http.max_connections <MAX_CONNECTIONS>
                                      Concurrent connections, or none [default: none]
                                      [env: FILESERVER__HTTP__MAX_CONNECTIONS]
      --log.level <LEVEL>             Least severe level that is logged [default: info]
                                      [possible values: trace, debug, info, warn, error]
                                      [env: FILESERVER__LOG__LEVEL]
  -v, --verbose                       Log every request [env: FILESERVER__LOG__VERBOSE]
      --log.color                     Color the log [default: true]
  -h, --help                          Print help
  -V, --version                       Print version
)");

        bool flag{};
        config::command wide{"app"};
        wide.option(std::string(30, 'a'), flag, {.help = "Beside"});
        wide.option(std::string(31, 'b'), flag, {.help = "Below"});

        EXPECT_EQ(wide.help(), R"(Usage: app [OPTIONS]

Options:
      --aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  Beside
      --bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
                                        Below
  -h, --help                            Print help
)");
    }

    TEST(config_help, report_prints_and_suggests_an_exit_code) {
        settings bound;
        config::command app{"fileserver", {.help = "Serves a directory over HTTP", .version = "1.2.0"}};
        declare(app, bound);

        testing::internal::CaptureStdout();
        EXPECT_EQ(app.report(rei::error{config::error_code::help_requested}), 0);
        EXPECT_EQ(testing::internal::GetCapturedStdout(), app.help());

        testing::internal::CaptureStdout();
        EXPECT_EQ(app.report(rei::error{config::error_code::version_requested}), 0);
        EXPECT_EQ(testing::internal::GetCapturedStdout(), "fileserver 1.2.0\n");

        const auto read = tests::read(app, {"--http.prot", "80"});
        ASSERT_FALSE(read);

        testing::internal::CaptureStderr();
        EXPECT_EQ(app.report(read.error()), 2);
        EXPECT_EQ(testing::internal::GetCapturedStderr(),
                  "error: unknown option '--http.prot'; did you mean '--http.port'?\n"
                  "\n"
                  "Usage: fileserver [OPTIONS] <ROOT> [MOUNT]...\n"
                  "\n"
                  "For more information, try '--help'.\n");

        const config::layer file{
            {.key = "root", .text = "site", .origin = {.source = "site.toml", .line = 1}},
            {.key = "http.prot", .text = "80", .origin = {.source = "site.toml", .line = 2}},
        };

        testing::internal::CaptureStderr();
        EXPECT_TRUE(app.apply(std::array{file}));
        EXPECT_EQ(testing::internal::GetCapturedStderr(),
                  "warning: site.toml:2: unknown key 'http.prot'; did you mean 'http.port'?\n");
    }
} // namespace rei::config::tests
