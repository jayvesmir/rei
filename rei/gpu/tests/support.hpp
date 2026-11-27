#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/error.hpp"
#include "rei/core/tests.hpp"
#include "rei/gpu/vk.hpp"

namespace rei::gpu::tests {
    using pixel = std::array<std::uint8_t, 4>;

    template <typename ValueType>
    auto has_error(const std::expected<ValueType, rei::error>& result, vk::error_code expected,
                   std::string_view mentioned) -> testing::AssertionResult {
        if (result) {
            return testing::AssertionFailure() << "operation succeeded";
        }
        if (result.error().code() != expected || !result.error().detail().contains(mentioned)) {
            return testing::AssertionFailure() << "unexpected error " << result.error();
        }

        return testing::AssertionSuccess();
    }

    template <typename ValueType> auto read(const vk::buffer& buffer) -> std::vector<ValueType> {
        auto values = std::vector<ValueType>(buffer.size() / sizeof(ValueType));
        EXPECT_TRUE(buffer.read(std::as_writable_bytes(std::span{values})));

        return values;
    }

    class vulkan : public testing::Test {
      private:
        struct log {
            std::mutex mutex;
            std::vector<std::string> messages;
            std::vector<std::string> trace;
        };

        std::shared_ptr<vulkan::log> m_log{std::make_shared<vulkan::log>()};
        std::optional<vk::instance> m_instance;
        std::optional<vk::surface> m_surface;
        std::optional<vk::device> m_device;

        static auto _unavailable(const rei::error& error) -> bool {
            const auto* const required = std::getenv("REI_GPU_VK_REQUIRE_DRIVER");
            const auto code = error.code();

            return (required == nullptr || std::string_view{required} != "1") &&
                   (code == vk::error_code::loader_not_found || code == vk::error_code::missing_layer ||
                    code == vk::error_code::missing_extension || code == vk::error_code::no_suitable_device);
        }

      protected:
        auto SetUp() -> void override {
            auto instance = vk::instance::make({
                .validation = vk::validation::full,
                .trace = true,
                .on_message = [log = m_log](const vk::message& message) -> void {
                    const auto lock = std::scoped_lock{log->mutex};

                    if (message.severity == vk::severity::trace) {
                        log->trace.emplace_back(message.text);
                    } else if (message.severity >= vk::severity::warning) {
                        log->messages.push_back(std::format("{}: {}", message.id, message.text));
                    }
                },
            });
            if (!instance && _unavailable(instance.error())) {
                GTEST_SKIP() << instance.error();
            }
            ASSERT_TRUE(instance) << instance.error();

            if (auto surface = vk::surface::headless(*instance)) {
                m_surface = std::move(*surface);
            }

            auto device =
                vk::device::make(*instance, {
                                                .prefer = {vk::feature::fragment_stores_and_atomics},
                                                .surface = m_surface,
                                            });
            if (!device && _unavailable(device.error())) {
                GTEST_SKIP() << device.error();
            }
            ASSERT_TRUE(device) << device.error();

            m_instance = std::move(*instance);
            m_device = std::move(*device);
        }

        auto TearDown() -> void override {
            m_device.reset();
            m_surface.reset();
            m_instance.reset();

            if (!IsSkipped()) {
                EXPECT_TRUE(m_log->messages.empty()) << testing::PrintToString(m_log->messages);
            }
        }

        [[nodiscard]] auto instance() const -> const vk::instance& { return *m_instance; }
        [[nodiscard]] auto device() const -> const vk::device& { return *m_device; }
        [[nodiscard]] auto surface() const -> const std::optional<vk::surface>& { return m_surface; }

        [[nodiscard]] auto take_messages() -> std::vector<std::string> {
            const auto lock = std::scoped_lock{m_log->mutex};
            return std::exchange(m_log->messages, {});
        }

        [[nodiscard]] auto take_trace() -> std::vector<std::string> {
            const auto lock = std::scoped_lock{m_log->mutex};
            return std::exchange(m_log->trace, {});
        }

        template <typename RecordType>
        [[nodiscard]] auto misuse(const RecordType& record, std::string_view mentioned) const
            -> testing::AssertionResult {
            const auto queue = device().queue();
            auto commands = queue.record();
            record(commands);

            return tests::has_error(queue.submit(std::move(commands)), vk::error_code::invalid_argument,
                                    mentioned);
        }
    };
} // namespace rei::gpu::tests
