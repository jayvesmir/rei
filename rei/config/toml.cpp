#include <array>
#include <cerrno>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <share.h>
#endif

#include "rei/config/config.hpp"
#include "rei/config/detail.hpp"
#include "rei/config/error_code.hpp"
#include "rei/core/error.hpp"
#include "rei/formats/toml.hpp"

namespace rei::config {
    namespace {
        namespace toml = formats::toml;

        struct closer {
            auto operator()(std::FILE* file) const noexcept -> void { std::fclose(file); }
        };

        auto open(const std::filesystem::path& path) -> std::unique_ptr<std::FILE, closer> {
#if defined(_WIN32)
            return std::unique_ptr<std::FILE, closer>{_wfsopen(path.c_str(), L"rb", _SH_DENYNO)};
#else
            return std::unique_ptr<std::FILE, closer>{std::fopen(path.c_str(), "rb")};
#endif
        }

        class flattener {
          private:
            std::string_view m_source;
            config::layer m_layer;

            [[nodiscard]] auto _origin(const toml::value& value) const -> config::origin {
                return {.source = std::string{m_source}, .line = value.line().value_or(0)};
            }

            [[nodiscard]] auto _unsupported(std::string_view key, const toml::value& value,
                                            std::string_view what) const -> std::unexpected<rei::error> {
                return std::unexpected(
                    rei::error{error_code::unsupported_value,
                               std::format("{}{}: {} is not supported", detail::origin_prefix(_origin(value)),
                                           key, what)});
            }

            auto _add(std::string_view key, const toml::value& value) -> std::expected<void, rei::error> {
                std::string text;

                if (const auto held = value.as_string()) {
                    text = *held;
                } else if (auto written = toml::serialize(value)) {
                    text = *std::move(written);
                } else {
                    return std::unexpected(std::move(written).error());
                }

                m_layer.push_back(
                    {.key = std::string{key}, .text = std::move(text), .origin = _origin(value)});
                return {};
            }

            auto _array(std::string_view key, const toml::value& array, std::span<const toml::value> items)
                -> std::expected<void, rei::error> {
                if (items.empty()) {
                    return _unsupported(key, array, "an empty array");
                }

                for (const auto& item : items) {
                    if (item.kind() == toml::kind::table) {
                        return _unsupported(key, array, "an array of tables");
                    }
                    if (item.kind() == toml::kind::array) {
                        return _unsupported(key, array, "an array of arrays");
                    }
                    if (auto added = _add(key, item); !added) {
                        return added;
                    }
                }

                return {};
            }

            auto _table(std::string_view prefix, const toml::table& table)
                -> std::expected<void, rei::error> {
                for (const auto& [name, value] : table) {
                    const auto key = prefix.empty() ? name : std::format("{}.{}", prefix, name);
                    std::expected<void, rei::error> added;

                    if (const auto nested = value.as_table()) {
                        added = _table(key, *nested);
                    } else if (const auto items = value.as_array()) {
                        added = _array(key, value, *items);
                    } else {
                        added = _add(key, value);
                    }

                    if (!added) {
                        return added;
                    }
                }

                return {};
            }

          public:
            explicit flattener(std::string_view source) : m_source(source) {}

            auto flatten(const toml::table& document) && -> std::expected<config::layer, rei::error> {
                return _table({}, document).transform([&] -> config::layer { return std::move(m_layer); });
            }
        };
    } // namespace

    auto read_toml(const std::filesystem::path& path) -> std::expected<config::layer, rei::error> {
        const auto source = path.string();
        const auto failed = [&] -> std::unexpected<rei::error> {
            const std::error_code code{errno != 0 ? errno : EIO, std::generic_category()};
            return std::unexpected(rei::error{code, std::format("{}: {}", source, code.message())});
        };

        errno = 0;

        const auto file = open(path);
        if (!file) {
            return failed();
        }

        std::string text;
        std::array<char, 16uz * 1024> chunk{};

        while (std::feof(file.get()) == 0) {
            const auto count = std::fread(chunk.data(), 1, chunk.size(), file.get());
            if (std::ferror(file.get()) != 0) {
                return failed();
            }

            text.append(chunk.data(), count);
        }

        return config::parse_toml(text, source);
    }

    auto parse_toml(std::string_view text, std::string_view source)
        -> std::expected<config::layer, rei::error> {
        const auto document = toml::parse(text);

        if (!document) {
            const auto& error = document.error();
            return std::unexpected(rei::error{
                error.code(), source.empty() ? error.detail() : std::format("{}:{}", source, error.detail()),
                error.location()});
        }

        return flattener{source}.flatten(*document);
    }
} // namespace rei::config
