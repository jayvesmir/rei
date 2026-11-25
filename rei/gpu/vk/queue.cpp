#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    auto queue::record(std::string_view name) const -> vk::commands {
        auto& device = m_state->device;

        auto lock = std::unique_lock{device.mutex};
        const auto serial = device.next_serial++;
        device.unfinished.emplace(serial, 0);
        lock.unlock();

        auto state = std::make_unique<commands::state>(std::shared_ptr<device::state>{m_state, &device},
                                                       serial, std::string{name});

        auto pool = device.take_pool();
        if (!pool) {
            state->fail(pool.error().code(),
                        std::format("record {}: {}", detail::label(name, "commands"), pool.error().detail()));
            return vk::commands{std::move(state)};
        }
        state->pool = *pool;

        const auto begin = VkCommandBufferBeginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        if (const auto result = device.table.vkBeginCommandBuffer(state->pool.buffer, &begin);
            result != VK_SUCCESS) {
            const auto error = detail::vulkan_error(
                result, "vkBeginCommandBuffer", std::format("record {}", detail::label(name, "commands")));
            state->fail(error.code(), error.detail());
            return vk::commands{std::move(state)};
        }

        device.name(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<std::uint64_t>(state->pool.buffer), name);

        return vk::commands{std::move(state)};
    }

    auto queue::submit(vk::commands commands, std::initializer_list<vk::ticket> after) const
        -> std::expected<vk::ticket, rei::error> {
        auto& queue = *m_state;
        auto& device = queue.device;

        if (!commands.m_state) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                "submit of commands that were already submitted or moved from",
            });
        }

        auto& recorded = *commands.m_state;
        if (recorded.device.get() != &device) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("submit of {} to another device's queue",
                            detail::label(recorded.name, "commands")),
            });
        }

        for (const auto& ticket : after) {
            if (ticket.queue != 0 || ticket.value > queue.submitted) {
                return std::unexpected(rei::error{
                    vk::error_code::invalid_argument,
                    std::format("submit of {} waits for ticket {}, which was never issued",
                                detail::label(recorded.name, "commands"), ticket.value),
                });
            }
        }

        if (recorded.compute) {
            recorded.fail(vk::error_code::invalid_argument,
                          std::format("submit of {}: {} is still open",
                                      detail::label(recorded.name, "commands"), recorded.compute->label));
        }
        if (!recorded.failure) {
            if (!recorded.pending.empty()) {
                recorded.begin("end");
                recorded.flush("end");
                recorded.end();
            }

            if (const auto result = device.table.vkEndCommandBuffer(recorded.pool.buffer);
                result != VK_SUCCESS) {
                const auto error = detail::vulkan_error(
                    result, "vkEndCommandBuffer",
                    std::format("submit of {}", detail::label(recorded.name, "commands")));
                recorded.fail(error.code(), error.detail());
            }
        }
        if (recorded.failure) {
            return std::unexpected(std::move(*recorded.failure));
        }

        auto lock = std::unique_lock{queue.mutex};

        auto taken = std::vector<std::weak_ptr<image::state>>{};
        {
            const auto device_lock = std::scoped_lock{device.mutex};
            taken = std::exchange(device.new_images, {});
        }

        const auto main = recorded.pool;
        auto items = std::exchange(recorded.staging, {});
        items.push_back({.pool = std::exchange(recorded.pool, {})});
        recorded.settled = true;

        const auto abandon = [&](rei::error error) -> std::unexpected<rei::error> {
            device.end_commands(recorded.serial, std::nullopt, std::move(items));
            {
                const auto device_lock = std::scoped_lock{device.mutex};
                device.new_images.append_range(taken);
            }
            return std::unexpected(std::move(error));
        };

        auto fresh = std::vector<std::shared_ptr<image::state>>{};
        for (const auto& weak : taken) {
            if (auto image = weak.lock()) {
                fresh.push_back(std::move(image));
            }
        }

        auto buffers = std::vector<VkCommandBufferSubmitInfo>{};
        if (!fresh.empty()) {
            const auto prefix = device.take_pool();
            if (!prefix) {
                return abandon(prefix.error());
            }
            items.push_back({.pool = *prefix});

            const auto barriers =
                fresh |
                std::views::transform(
                    [](const std::shared_ptr<image::state>& image) -> VkImageMemoryBarrier2 {
                        const auto home = detail::use_of(image->home);
                        return {
                            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                            .dstStageMask = home.stages,
                            .dstAccessMask = home.access,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = home.layout,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = image->handle,
                            .subresourceRange = image->range(),
                        };
                    }) |
                std::ranges::to<std::vector>();
            const auto dependency = VkDependencyInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
                .pImageMemoryBarriers = barriers.data(),
            };
            const auto begin = VkCommandBufferBeginInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };

            if (const auto result = device.table.vkBeginCommandBuffer(prefix->buffer, &begin);
                result != VK_SUCCESS) {
                return abandon(detail::vulkan_error(result, "vkBeginCommandBuffer"));
            }
            device.table.vkCmdPipelineBarrier2(prefix->buffer, &dependency);
            if (const auto result = device.table.vkEndCommandBuffer(prefix->buffer); result != VK_SUCCESS) {
                return abandon(detail::vulkan_error(result, "vkEndCommandBuffer"));
            }

            buffers.push_back(
                {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = prefix->buffer});
        }
        buffers.push_back(
            {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = main.buffer});

        const auto waits = after |
                           std::views::transform([&](const vk::ticket& ticket) -> VkSemaphoreSubmitInfo {
                               return {
                                   .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = queue.timeline,
                                   .value = ticket.value,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               };
                           }) |
                           std::ranges::to<std::vector>();

        const auto value = queue.submitted + 1;
        const auto signal = VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = queue.timeline,
            .value = value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const auto submit = VkSubmitInfo2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()),
            .pWaitSemaphoreInfos = waits.data(),
            .commandBufferInfoCount = static_cast<std::uint32_t>(buffers.size()),
            .pCommandBufferInfos = buffers.data(),
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signal,
        };

        if (const auto result = device.table.vkQueueSubmit2(queue.handle, 1, &submit, VK_NULL_HANDLE);
            result != VK_SUCCESS) {
            return abandon(detail::vulkan_error(result, "vkQueueSubmit2"));
        }

        queue.submitted = value;
        device.end_commands(recorded.serial, value, std::move(items));
        lock.unlock();

        if (device.tracing) {
            const auto first_uses =
                fresh | std::views::transform([](const std::shared_ptr<image::state>& image) -> std::string {
                    return std::format("{} undefined → {}", detail::label(image->name, "image"),
                                       detail::use_of(image->home).name);
                }) |
                std::views::join_with(std::string_view{", "}) | std::ranges::to<std::string>();

            device.trace("submit {}: {}ticket {}", detail::label(recorded.name, "commands"),
                         first_uses.empty() ? "" : std::format("first use: {}; ", first_uses), value);
        }

        fresh.clear();
        device.collect();

        return vk::ticket{.queue = 0, .value = value};
    }
} // namespace rei::gpu::vk
