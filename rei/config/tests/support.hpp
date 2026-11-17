#pragma once

#include <array>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/config/config.hpp"
#include "rei/config/error_code.hpp"
#include "rei/core/error.hpp"
#include "rei/core/tests.hpp"

namespace rei::config::tests {
    template <typename Value>
    auto fails_with(const std::expected<Value, rei::error>& result, std::error_code code,
                    std::string_view detail) -> testing::AssertionResult {
        if (result) {
            return testing::AssertionFailure() << "operation succeeded";
        }
        if (result.error().code() != code || result.error().detail() != detail) {
            return testing::AssertionFailure() << "unexpected error " << result.error();
        }

        return testing::AssertionSuccess();
    }

    inline auto describe(const config::layer& layer) -> std::vector<std::string> {
        return std::ranges::to<std::vector>(
            layer | std::views::transform([](const config::value& value) -> std::string {
                return value.origin.line == 0
                           ? std::format("{}={} ({})", value.key, value.text, value.origin.source)
                           : std::format("{}={} ({}:{})", value.key, value.text, value.origin.source,
                                         value.origin.line);
            }));
    }

    inline auto read(config::command& app, const std::vector<std::string_view>& args)
        -> std::expected<config::layer, rei::error> {
        return app.read_args(args);
    }

    inline auto load(config::command& app, const std::vector<std::string_view>& args)
        -> std::expected<void, rei::error> {
        return app.read_args(args).and_then([&](config::layer&& layer) -> std::expected<void, rei::error> {
            return app.apply(std::array{std::move(layer)});
        });
    }

    inline auto set(config::command& app, std::string_view key, std::string_view text)
        -> std::expected<void, rei::error> {
        return app.apply(std::array{config::layer{{.key = std::string{key}, .text = std::string{text}}}});
    }
} // namespace rei::config::tests
