#include <algorithm>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    namespace {
        auto overlaps(std::uint32_t first, std::uint32_t count, std::uint32_t other_first,
                      std::uint32_t other_count) noexcept -> bool {
            return first < other_first + other_count && other_first < first + count;
        }

        auto same(const VkImageSubresourceRange& left, const VkImageSubresourceRange& right) noexcept
            -> bool {
            return left.aspectMask == right.aspectMask && left.baseMipLevel == right.baseMipLevel &&
                   left.levelCount == right.levelCount && left.baseArrayLayer == right.baseArrayLayer &&
                   left.layerCount == right.layerCount;
        }

        auto overlapping(const detail::barrier& left, const detail::barrier& right) noexcept -> bool {
            if (left.buffer != VK_NULL_HANDLE || right.buffer != VK_NULL_HANDLE) {
                return left.buffer == right.buffer;
            }

            return left.image == right.image && (left.range.aspectMask & right.range.aspectMask) != 0 &&
                   overlaps(left.range.baseMipLevel, left.range.levelCount, right.range.baseMipLevel,
                            right.range.levelCount) &&
                   overlaps(left.range.baseArrayLayer, left.range.layerCount, right.range.baseArrayLayer,
                            right.range.layerCount);
        }

        auto part(std::string_view singular, std::string_view plural, std::uint32_t first,
                  std::uint32_t count, std::uint32_t total) -> std::string {
            if (total == 1 || count == total) {
                return {};
            }
            if (count == 1) {
                return std::format(" {} {}", singular, first);
            }

            return std::format(" {} {}-{}", plural, first, first + count - 1);
        }

        auto describe(const detail::barrier& barrier) -> std::string {
            return std::format("{}: {} → {}{}{}", barrier.target, barrier.from.name, barrier.to.name,
                               barrier.discard ? ", discard" : "", barrier.folded ? " (folded)" : "");
        }

        auto buffer_use(vk::access access) noexcept -> std::optional<detail::use> {
            auto use = detail::use_of(access);
            use.layout = VK_IMAGE_LAYOUT_UNDEFINED;

            switch (access) {
            case vk::access::undefined:
            case vk::access::transfer_src:
            case vk::access::transfer_dst:
                return use;
            case vk::access::shader_read:
            case vk::access::shader_write:
                use.access |= VK_ACCESS_2_UNIFORM_READ_BIT;
                return use;
            default:
                return std::nullopt;
            }
        }
    } // namespace

    commands::state::state(std::shared_ptr<device::state> device, std::uint64_t serial,
                           std::string name) noexcept
        : device(std::move(device)), serial(serial), name(std::move(name)) {}

    commands::state::~state() {
        if (settled) {
            return;
        }

        auto items = std::move(staging);
        if (pool.pool != VK_NULL_HANDLE) {
            items.push_back({.pool = pool});
        }
        device->end_commands(serial, std::nullopt, std::move(items));
    }

    auto commands::state::label() const -> std::string {
        return detail::label(name, "commands");
    }

    auto commands::state::fail(std::error_code code, std::string detail) -> void {
        if (!failure) {
            failure.emplace(code, std::move(detail));
        }
    }

    auto commands::state::begin(std::string_view workflow) const -> void {
        if (!device->labeling) {
            return;
        }

        const auto text = std::format("rei: {}", workflow);
        const auto label = VkDebugUtilsLabelEXT{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pLabelName = text.c_str(),
        };
        device->instance.raw().vkCmdBeginDebugUtilsLabelEXT(pool.buffer, &label);
    }

    auto commands::state::end() const -> void {
        if (device->labeling) {
            device->instance.raw().vkCmdEndDebugUtilsLabelEXT(pool.buffer);
        }
    }

    auto commands::state::add(std::string_view workflow, detail::barrier barrier) -> void {
        const auto folding = std::ranges::find_if(pending, [&](const detail::barrier& other) -> bool {
            return other.buffer == barrier.buffer && other.image == barrier.image &&
                   same(other.range, barrier.range) && other.to == barrier.from;
        });
        if (folding != pending.end()) {
            folding->to = barrier.to;
            folding->discard = folding->discard || barrier.discard;
            folding->folded = true;
            return;
        }

        if (std::ranges::any_of(
                pending, [&](const detail::barrier& other) -> bool { return overlapping(other, barrier); })) {
            flush(workflow);
        }
        pending.push_back(std::move(barrier));
    }

    auto commands::state::flush(std::string_view workflow) -> void {
        if (pending.empty()) {
            return;
        }

        auto images = std::vector<VkImageMemoryBarrier2>{};
        auto buffers = std::vector<VkBufferMemoryBarrier2>{};
        for (const auto& barrier : pending) {
            if (barrier.image != VK_NULL_HANDLE) {
                images.push_back({
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = barrier.from.stages,
                    .srcAccessMask = barrier.from.access,
                    .dstStageMask = barrier.to.stages,
                    .dstAccessMask = barrier.to.access,
                    .oldLayout = barrier.discard ? VK_IMAGE_LAYOUT_UNDEFINED : barrier.from.layout,
                    .newLayout = barrier.to.layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = barrier.image,
                    .subresourceRange = barrier.range,
                });
            } else {
                buffers.push_back({
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = barrier.from.stages,
                    .srcAccessMask = barrier.from.access,
                    .dstStageMask = barrier.to.stages,
                    .dstAccessMask = barrier.to.access,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = barrier.buffer,
                    .offset = 0,
                    .size = VK_WHOLE_SIZE,
                });
            }
        }

        const auto dependency = VkDependencyInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = static_cast<std::uint32_t>(buffers.size()),
            .pBufferMemoryBarriers = buffers.data(),
            .imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size()),
            .pImageMemoryBarriers = images.data(),
        };
        device->table.vkCmdPipelineBarrier2(pool.buffer, &dependency);

        if (device->tracing) {
            trace(workflow, "barrier {}",
                  pending | std::views::transform(describe) | std::views::join_with(std::string_view{"; "}) |
                      std::ranges::to<std::string>());
        }
        pending.clear();
    }

    auto commands::state::finish() -> void {
        if (pending.empty()) {
            return;
        }

        begin("end");
        flush("end");
        end();
    }

    commands::commands(std::unique_ptr<commands::state> state) noexcept : m_state(std::move(state)) {}

    commands::commands(commands&& other) noexcept = default;

    auto commands::operator=(commands&& other) noexcept -> commands& = default;

    commands::~commands() = default;

    auto commands::barrier(std::initializer_list<vk::transition> transitions) -> void {
        barrier(std::span{transitions.begin(), transitions.size()});
    }

    auto commands::barrier(std::span<const vk::transition> transitions) -> void {
        auto& self = *m_state;

        for (const auto& transition : transitions) {
            if (self.failure) {
                return;
            }
            if (transition.from == vk::access::manual || transition.to == vk::access::manual) {
                self.fail(vk::error_code::invalid_argument,
                          "barrier: manual is a home, not an access a barrier can go from or to");
                return;
            }

            if (transition.target.m_buffer) {
                const auto& target = transition.target.m_buffer->get();
                if (!self.usable(target.m_state, "barrier on a buffer", "buffer")) {
                    return;
                }

                const auto& buffer = *target.m_state;
                const auto before = buffer_use(transition.from);
                const auto after = buffer_use(transition.to);
                if (!before || !after) {
                    self.fail(vk::error_code::invalid_argument,
                              std::format("barrier on {}: {} → {}, and a buffer has no such access",
                                          detail::quoted(buffer.name, "buffer"),
                                          detail::use_of(transition.from).name,
                                          detail::use_of(transition.to).name));
                    return;
                }

                self.add("barrier", {
                                        .target = detail::label(buffer.name, "buffer"),
                                        .buffer = buffer.handle,
                                        .from = *before,
                                        .to = *after,
                                    });
                continue;
            }

            const auto& target = transition.target.m_view->get();
            if (!self.usable(target.m_state, "barrier on an image view", "image view")) {
                return;
            }

            const auto& view = *target.m_state;
            const auto image = view.image.lock();
            if (!image) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("barrier on {}: {} was destroyed",
                                      detail::quoted(view.own ? view.image_name : view.name,
                                                     view.own ? "image" : "image view"),
                                      view.own ? "it" : detail::quoted(view.image_name, "its image")));
                return;
            }

            self.add("barrier", {
                                    .target = detail::label(image->name, "image") +
                                              part("mip", "mips", view.range.baseMipLevel,
                                                   view.range.levelCount, image->mip_levels) +
                                              part("layer", "layers", view.range.baseArrayLayer,
                                                   view.range.layerCount, image->array_layers),
                                    .image = image->handle,
                                    .range = view.range,
                                    .from = detail::use_of(transition.from),
                                    .to = detail::use_of(transition.to),
                                });
        }
    }

    auto commands::handle() -> VkCommandBuffer {
        auto& self = *m_state;

        if (!self.failure && !self.pending.empty()) {
            self.begin("raw");
            self.flush("raw");
            self.end();
        }

        return self.pool.buffer;
    }
} // namespace rei::gpu::vk
