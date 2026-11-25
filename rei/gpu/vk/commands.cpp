#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/formats.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    namespace {
        auto level_size(const detail::format_info& format, vk::extent extent) noexcept -> std::uint64_t {
            const auto columns = (std::uint64_t{extent.width} + format.block.width - 1) / format.block.width;
            const auto rows = (std::uint64_t{extent.height} + format.block.height - 1) / format.block.height;
            const auto slices = (std::uint64_t{extent.depth} + format.block.depth - 1) / format.block.depth;

            return columns * rows * slices * format.block_size;
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

    auto commands::state::fail(std::error_code code, std::string detail) -> void {
        if (!failure) {
            failure.emplace(code, std::move(detail));
        }
    }

    auto commands::state::outside(std::string_view call) -> bool {
        if (compute) {
            fail(vk::error_code::invalid_argument, std::format("{}: {} is still open", call, compute->label));
        } else if (render) {
            fail(vk::error_code::invalid_argument, std::format("{}: {} is still open", call, render->label));
        }

        return !failure;
    }

    auto commands::state::alive(std::string_view call, const image_view::state& view)
        -> std::shared_ptr<image::state> {
        auto image = view.image.lock();
        if (!image) {
            fail(vk::error_code::invalid_argument,
                 std::format("{}: {} was destroyed", call,
                             view.own ? "it" : detail::quoted(view.image_name, "its image")));
        }

        return image;
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
                   other.range.aspectMask == barrier.range.aspectMask &&
                   other.range.baseMipLevel == barrier.range.baseMipLevel &&
                   other.range.levelCount == barrier.range.levelCount &&
                   other.range.baseArrayLayer == barrier.range.baseArrayLayer &&
                   other.range.layerCount == barrier.range.layerCount && other.to == barrier.from;
        });
        if (folding != pending.end()) {
            folding->to = barrier.to;
            folding->discard = folding->discard || barrier.discard;
            folding->folded = true;
            return;
        }

        const auto overlapping = std::ranges::any_of(pending, [&](const detail::barrier& other) -> bool {
            if (other.buffer != VK_NULL_HANDLE || barrier.buffer != VK_NULL_HANDLE) {
                return other.buffer == barrier.buffer;
            }

            const auto& left = other.range;
            const auto& right = barrier.range;
            return other.image == barrier.image && (left.aspectMask & right.aspectMask) != 0 &&
                   left.baseMipLevel < right.baseMipLevel + right.levelCount &&
                   right.baseMipLevel < left.baseMipLevel + left.levelCount &&
                   left.baseArrayLayer < right.baseArrayLayer + right.layerCount &&
                   right.baseArrayLayer < left.baseArrayLayer + left.layerCount;
        });
        if (overlapping) {
            flush(workflow);
        }

        pending.push_back(std::move(barrier));
    }

    auto commands::state::flush(std::string_view workflow) -> void {
        if (pending.empty()) {
            return;
        }

        auto memory = std::vector<VkMemoryBarrier2>{};
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
            } else if (barrier.buffer != VK_NULL_HANDLE) {
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
            } else {
                memory.push_back({
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = barrier.from.stages,
                    .srcAccessMask = barrier.from.access,
                    .dstStageMask = barrier.to.stages,
                    .dstAccessMask = barrier.to.access,
                });
            }
        }

        const auto dependency = VkDependencyInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = static_cast<std::uint32_t>(memory.size()),
            .pMemoryBarriers = memory.data(),
            .bufferMemoryBarrierCount = static_cast<std::uint32_t>(buffers.size()),
            .pBufferMemoryBarriers = buffers.data(),
            .imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size()),
            .pImageMemoryBarriers = images.data(),
        };
        device->table.vkCmdPipelineBarrier2(pool.buffer, &dependency);

        if (device->tracing) {
            trace(workflow, "barrier {}",
                  pending | std::views::transform([](const detail::barrier& barrier) -> std::string {
                      return std::format("{}: {} → {}{}{}", barrier.target, barrier.from.name,
                                         barrier.to.name, barrier.discard ? ", discard" : "",
                                         barrier.folded ? " (folded)" : "");
                  }) | std::views::join_with(std::string_view{"; "}) |
                      std::ranges::to<std::string>());
        }
        pending.clear();
    }

    auto commands::state::enter(std::string_view workflow, const image::state& image,
                                const VkImageSubresourceRange& range, vk::access needed, bool discard)
        -> void {
        if (image.home == vk::access::manual || image.home == needed) {
            return;
        }

        add(workflow, {
                          .target = image.label(range),
                          .image = image.handle,
                          .range = range,
                          .from = detail::use_of(image.home),
                          .to = detail::use_of(needed),
                          .discard = discard,
                      });
    }

    auto commands::state::leave(std::string_view workflow, const image::state& image,
                                const VkImageSubresourceRange& range, vk::access needed, bool wrote) -> void {
        if (image.home == vk::access::manual || (image.home == needed && !wrote)) {
            return;
        }

        add(workflow, {
                          .target = image.label(range),
                          .image = image.handle,
                          .range = range,
                          .from = detail::use_of(needed),
                          .to = detail::use_of(image.home),
                      });
    }

    auto commands::state::enter(std::string_view workflow, const buffer::state& buffer) -> void {
        add(workflow, {
                          .target = detail::label(buffer.name, "buffer"),
                          .buffer = buffer.handle,
                          .from = detail::home_of(buffer.usage, buffer.host),
                          .to = detail::use_of(vk::access::transfer_dst),
                      });
    }

    auto commands::state::leave(std::string_view workflow, const buffer::state& buffer) -> void {
        add(workflow, {
                          .target = detail::label(buffer.name, "buffer"),
                          .buffer = buffer.handle,
                          .from = detail::use_of(vk::access::transfer_dst),
                          .to = detail::home_of(buffer.usage, buffer.host),
                      });
    }

    auto commands::state::stage(std::string_view call, std::string_view workflow,
                                std::span<const std::byte> data) -> std::optional<VkBuffer> {
        const auto info = VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = data.size(),
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const auto allocation_info = VmaAllocationCreateInfo{
            .flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        auto staged = detail::retired{.label = "staging"};
        auto allocated = VmaAllocationInfo{};
        if (const auto result = vmaCreateBuffer(device->allocator, &info, &allocation_info, &staged.buffer,
                                                &staged.allocation, &allocated);
            result != VK_SUCCESS) {
            const auto error =
                detail::vulkan_error(result, "vmaCreateBuffer",
                                     std::format("{}: no staging buffer of {} bytes", call, data.size()));
            fail(error.code(), error.detail());
            return std::nullopt;
        }
        staging.push_back(staged);

        std::ranges::copy(data, static_cast<std::byte*>(allocated.pMappedData));
        if (const auto result = vmaFlushAllocation(device->allocator, staged.allocation, 0, VK_WHOLE_SIZE);
            result != VK_SUCCESS) {
            const auto error = detail::vulkan_error(result, "vmaFlushAllocation", call);
            fail(error.code(), error.detail());
            return std::nullopt;
        }

        device->name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(staged.buffer),
                     std::format("staging for {}", workflow));
        trace(workflow, "staging buffer, {} bytes", data.size());

        return staged.buffer;
    }

    commands::commands(std::unique_ptr<commands::state> state) noexcept : m_state(std::move(state)) {}

    commands::commands(commands&& other) noexcept = default;

    auto commands::operator=(commands&& other) noexcept -> commands& = default;

    commands::~commands() = default;

    auto commands::compute(const vk::compute_options& options) -> vk::compute_pass {
        auto& self = *m_state;

        if (self.outside("compute")) {
            auto& scope = self.compute.emplace();
            scope.label = detail::quoted(options.name, "compute pass");
            scope.workflow = options.name.empty() ? "compute" : std::format("compute {}", options.name);
            scope.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
            scope.concurrent = options.concurrent;

            self.begin(scope.workflow);
        }

        return vk::compute_pass{*this};
    }

    auto commands::upload(const vk::buffer& destination, std::span<const std::byte> data,
                          std::uint64_t offset) -> void {
        auto& self = *m_state;

        if (!self.outside("upload into a buffer") ||
            !self.usable(destination.m_state, "upload into a buffer", "buffer")) {
            return;
        }

        const auto& target = *destination.m_state;
        const auto call = std::format("upload into {}", detail::quoted(target.name, "buffer"));

        if (!vk::has(target.usage, vk::buffer_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: its usage lacks transfer_dst", call));
            return;
        }
        if (!detail::fits(target.size, offset, data.size())) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: {} bytes at offset {} go past its end at {}", call, data.size(),
                                  offset, target.size));
            return;
        }
        if (data.empty()) {
            return;
        }

        const auto workflow = std::format("upload {}", detail::label(target.name, "buffer"));
        self.begin(workflow);

        const auto staged = self.stage(call, workflow, data);
        if (!staged) {
            self.end();
            return;
        }

        self.enter(workflow, target);
        self.flush(workflow);

        const auto region = VkBufferCopy{.srcOffset = 0, .dstOffset = offset, .size = data.size()};
        self.device->table.vkCmdCopyBuffer(self.pool.buffer, *staged, target.handle, 1, &region);
        self.trace(workflow, "copy staging → {}", detail::label(target.name, "buffer"));

        self.leave(workflow, target);
        self.end();
    }

    auto commands::upload(const vk::image& destination, std::span<const std::byte> pixels,
                          const vk::image_region& region) -> void {
        auto& self = *m_state;

        if (!self.outside("upload into an image") ||
            !self.usable(destination.m_state, "upload into an image", "image")) {
            return;
        }

        const auto& target = *destination.m_state;
        const auto call = std::format("upload into {}", detail::quoted(target.name, "image"));
        const auto format = detail::info_of(target.format);

        if (!vk::has(target.usage, vk::image_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: its usage lacks transfer_dst", call));
            return;
        }
        if (region.buffer_offset != 0) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: region.buffer_offset is {}, and an upload takes its pixels whole",
                                  call, region.buffer_offset));
            return;
        }
        if (format.planar || !std::has_single_bit(format.aspects) || target.samples != 1) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("{}: copies into multisampled, multi-planar and depth-stencil images aren't "
                            "supported",
                            call));
            return;
        }
        if (region.mip >= target.mip_levels || region.layer >= target.array_layers) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: mip {} layer {} isn't in it, it has {} and {}", call, region.mip,
                                  region.layer, target.mip_levels, target.array_layers));
            return;
        }

        const auto extent = target.extent_of(region.mip);
        if (const auto expected = level_size(format, extent); pixels.size() != expected) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: {} bytes given, and mip {} of {} {}x{}x{} holds {}", call,
                                  pixels.size(), region.mip, format.name, extent.width, extent.height,
                                  extent.depth, expected));
            return;
        }

        const auto range = VkImageSubresourceRange{
            .aspectMask = format.aspects,
            .baseMipLevel = region.mip,
            .levelCount = 1,
            .baseArrayLayer = region.layer,
            .layerCount = 1,
        };
        const auto workflow = std::format("upload {}", detail::label(target.name, "image"));
        self.begin(workflow);

        const auto staged = self.stage(call, workflow, pixels);
        if (!staged) {
            self.end();
            return;
        }

        self.enter(workflow, target, range, vk::access::transfer_dst, true);
        self.flush(workflow);

        const auto copy = VkBufferImageCopy{
            .imageSubresource =
                {
                    .aspectMask = format.aspects,
                    .mipLevel = region.mip,
                    .baseArrayLayer = region.layer,
                    .layerCount = 1,
                },
            .imageExtent = {.width = extent.width, .height = extent.height, .depth = extent.depth},
        };
        self.device->table.vkCmdCopyBufferToImage(self.pool.buffer, *staged, target.handle,
                                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        self.trace(workflow, "copy staging → {}", target.label(range));

        self.leave(workflow, target, range, vk::access::transfer_dst, true);
        self.end();
    }

    auto commands::copy(const vk::buffer& source, const vk::buffer& destination,
                        const vk::buffer_region& region) -> void {
        auto& self = *m_state;

        if (!self.outside("copy from a buffer") ||
            !self.usable(source.m_state, "copy from a buffer", "buffer") ||
            !self.usable(destination.m_state, "copy into a buffer", "buffer")) {
            return;
        }

        const auto& origin = *source.m_state;
        const auto& target = *destination.m_state;
        const auto call = std::format("copy from {} into {}", detail::quoted(origin.name, "buffer"),
                                      detail::quoted(target.name, "buffer"));
        const auto size = region.size.value_or(origin.size - std::min(region.source_offset, origin.size));

        if (!vk::has(origin.usage, vk::buffer_usage::transfer_src) ||
            !vk::has(target.usage, vk::buffer_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the source needs the transfer_src usage, the destination transfer_dst",
                                  call));
            return;
        }
        if (!detail::fits(origin.size, region.source_offset, size) ||
            !detail::fits(target.size, region.destination_offset, size)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: {} bytes from offset {} to offset {} don't fit in {} and {} bytes",
                                  call, size, region.source_offset, region.destination_offset, origin.size,
                                  target.size));
            return;
        }
        if (origin.handle == target.handle && region.source_offset < region.destination_offset + size &&
            region.destination_offset < region.source_offset + size) {
            self.fail(vk::error_code::invalid_argument, std::format("{}: the two ranges overlap", call));
            return;
        }
        if (size == 0) {
            return;
        }

        const auto workflow = std::format("copy {} → {}", detail::label(origin.name, "buffer"),
                                          detail::label(target.name, "buffer"));
        self.begin(workflow);
        self.enter(workflow, target);
        self.flush(workflow);

        const auto copy = VkBufferCopy{
            .srcOffset = region.source_offset,
            .dstOffset = region.destination_offset,
            .size = size,
        };
        self.device->table.vkCmdCopyBuffer(self.pool.buffer, origin.handle, target.handle, 1, &copy);

        self.leave(workflow, target);
        self.end();
    }

    auto commands::copy(const vk::buffer& source, const vk::image& destination,
                        const vk::image_region& region) -> void {
        auto& self = *m_state;

        if (!self.outside("copy from a buffer") ||
            !self.usable(source.m_state, "copy from a buffer", "buffer") ||
            !self.usable(destination.m_state, "copy into an image", "image")) {
            return;
        }

        const auto& origin = *source.m_state;
        const auto& target = *destination.m_state;
        const auto call = std::format("copy from {} into {}", detail::quoted(origin.name, "buffer"),
                                      detail::quoted(target.name, "image"));
        const auto format = detail::info_of(target.format);

        if (!vk::has(origin.usage, vk::buffer_usage::transfer_src) ||
            !vk::has(target.usage, vk::image_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the source needs the transfer_src usage, the destination transfer_dst",
                                  call));
            return;
        }
        if (format.planar || !std::has_single_bit(format.aspects) || target.samples != 1) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("{}: copies into multisampled, multi-planar and depth-stencil images aren't "
                            "supported",
                            call));
            return;
        }
        if (region.mip >= target.mip_levels || region.layer >= target.array_layers) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: mip {} layer {} isn't in the image, it has {} and {}", call,
                                  region.mip, region.layer, target.mip_levels, target.array_layers));
            return;
        }

        const auto extent = target.extent_of(region.mip);
        const auto size = level_size(format, extent);
        const auto alignment = format.aspects == VK_IMAGE_ASPECT_COLOR_BIT ? format.block_size : 4U;
        if (region.buffer_offset % alignment != 0 || !detail::fits(origin.size, region.buffer_offset, size)) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("{}: mip {} needs {} bytes at an offset that is a multiple of {}, and the buffer "
                            "has {} at offset {}",
                            call, region.mip, size, alignment, origin.size, region.buffer_offset));
            return;
        }

        const auto range = VkImageSubresourceRange{
            .aspectMask = format.aspects,
            .baseMipLevel = region.mip,
            .levelCount = 1,
            .baseArrayLayer = region.layer,
            .layerCount = 1,
        };
        const auto workflow = std::format("copy {} → {}", detail::label(origin.name, "buffer"),
                                          detail::label(target.name, "image"));
        self.begin(workflow);
        self.enter(workflow, target, range, vk::access::transfer_dst, true);
        self.flush(workflow);

        const auto copy = VkBufferImageCopy{
            .bufferOffset = region.buffer_offset,
            .imageSubresource =
                {
                    .aspectMask = format.aspects,
                    .mipLevel = region.mip,
                    .baseArrayLayer = region.layer,
                    .layerCount = 1,
                },
            .imageExtent = {.width = extent.width, .height = extent.height, .depth = extent.depth},
        };
        self.device->table.vkCmdCopyBufferToImage(self.pool.buffer, origin.handle, target.handle,
                                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        self.leave(workflow, target, range, vk::access::transfer_dst, true);
        self.end();
    }

    auto commands::copy(const vk::image& source, const vk::buffer& destination,
                        const vk::image_region& region) -> void {
        auto& self = *m_state;

        if (!self.outside("copy from an image") ||
            !self.usable(source.m_state, "copy from an image", "image") ||
            !self.usable(destination.m_state, "copy into a buffer", "buffer")) {
            return;
        }

        const auto& origin = *source.m_state;
        const auto& target = *destination.m_state;
        const auto call = std::format("copy from {} into {}", detail::quoted(origin.name, "image"),
                                      detail::quoted(target.name, "buffer"));
        const auto format = detail::info_of(origin.format);

        if (!vk::has(origin.usage, vk::image_usage::transfer_src) ||
            !vk::has(target.usage, vk::buffer_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the source needs the transfer_src usage, the destination transfer_dst",
                                  call));
            return;
        }
        if (format.planar || !std::has_single_bit(format.aspects) || origin.samples != 1) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("{}: copies from multisampled, multi-planar and depth-stencil images aren't "
                            "supported",
                            call));
            return;
        }
        if (region.mip >= origin.mip_levels || region.layer >= origin.array_layers) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: mip {} layer {} isn't in the image, it has {} and {}", call,
                                  region.mip, region.layer, origin.mip_levels, origin.array_layers));
            return;
        }

        const auto extent = origin.extent_of(region.mip);
        const auto size = level_size(format, extent);
        const auto alignment = format.aspects == VK_IMAGE_ASPECT_COLOR_BIT ? format.block_size : 4U;
        if (region.buffer_offset % alignment != 0 || !detail::fits(target.size, region.buffer_offset, size)) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("{}: mip {} needs {} bytes at an offset that is a multiple of {}, and the buffer "
                            "has {} at offset {}",
                            call, region.mip, size, alignment, target.size, region.buffer_offset));
            return;
        }

        const auto range = VkImageSubresourceRange{
            .aspectMask = format.aspects,
            .baseMipLevel = region.mip,
            .levelCount = 1,
            .baseArrayLayer = region.layer,
            .layerCount = 1,
        };
        const auto workflow = std::format("copy {} → {}", detail::label(origin.name, "image"),
                                          detail::label(target.name, "buffer"));
        self.begin(workflow);
        self.enter(workflow, origin, range, vk::access::transfer_src, false);
        self.enter(workflow, target);
        self.flush(workflow);

        const auto copy = VkBufferImageCopy{
            .bufferOffset = region.buffer_offset,
            .imageSubresource =
                {
                    .aspectMask = format.aspects,
                    .mipLevel = region.mip,
                    .baseArrayLayer = region.layer,
                    .layerCount = 1,
                },
            .imageExtent = {.width = extent.width, .height = extent.height, .depth = extent.depth},
        };
        self.device->table.vkCmdCopyImageToBuffer(
            self.pool.buffer, origin.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.handle, 1, &copy);

        self.leave(workflow, origin, range, vk::access::transfer_src, false);
        self.leave(workflow, target);
        self.end();
    }

    auto commands::copy(const vk::image& source, const vk::image& destination) -> void {
        auto& self = *m_state;

        if (!self.outside("copy from an image") ||
            !self.usable(source.m_state, "copy from an image", "image") ||
            !self.usable(destination.m_state, "copy into an image", "image")) {
            return;
        }

        const auto& origin = *source.m_state;
        const auto& target = *destination.m_state;
        const auto call = std::format("copy from {} into {}", detail::quoted(origin.name, "image"),
                                      detail::quoted(target.name, "image"));
        const auto origin_format = detail::info_of(origin.format);
        const auto target_format = detail::info_of(target.format);

        if (!vk::has(origin.usage, vk::image_usage::transfer_src) ||
            !vk::has(target.usage, vk::image_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the source needs the transfer_src usage, the destination transfer_dst",
                                  call));
            return;
        }
        if (origin.handle == target.handle) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: an image can't be copied onto itself", call));
            return;
        }
        if (origin_format.block_size != target_format.block_size ||
            origin_format.block.width != target_format.block.width ||
            origin_format.block.height != target_format.block.height ||
            origin_format.aspects != target_format.aspects || origin_format.planar || target_format.planar) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: {} and {} aren't size-compatible", call, origin_format.name,
                                  target_format.name));
            return;
        }
        if (origin.extent.width != target.extent.width || origin.extent.height != target.extent.height ||
            origin.extent.depth != target.extent.depth || origin.mip_levels != target.mip_levels ||
            origin.array_layers != target.array_layers || origin.samples != target.samples) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the two differ in extent, mip levels, array layers or samples", call));
            return;
        }

        const auto workflow = std::format("copy {} → {}", detail::label(origin.name, "image"),
                                          detail::label(target.name, "image"));
        self.begin(workflow);
        self.enter(workflow, origin, origin.range(), vk::access::transfer_src, false);
        self.enter(workflow, target, target.range(), vk::access::transfer_dst, true);
        self.flush(workflow);

        auto regions = std::vector<VkImageCopy>{};
        for (auto mip = 0U; mip < origin.mip_levels; mip++) {
            const auto extent = origin.extent_of(mip);
            const auto layers = VkImageSubresourceLayers{
                .aspectMask = origin_format.aspects,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = origin.array_layers,
            };
            regions.push_back({
                .srcSubresource = layers,
                .dstSubresource = layers,
                .extent = {.width = extent.width, .height = extent.height, .depth = extent.depth},
            });
        }
        self.device->table.vkCmdCopyImage(
            self.pool.buffer, origin.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.handle,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());

        self.leave(workflow, origin, origin.range(), vk::access::transfer_src, false);
        self.leave(workflow, target, target.range(), vk::access::transfer_dst, true);
        self.end();
    }

    auto commands::barrier(std::initializer_list<vk::transition> transitions) -> void {
        barrier(std::span{transitions.begin(), transitions.size()});
    }

    auto commands::barrier(std::span<const vk::transition> transitions) -> void {
        auto& self = *m_state;

        if (!self.outside("barrier")) {
            return;
        }

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
            const auto image = self.alive(std::format("barrier on {}", view.quoted()), view);
            if (!image) {
                return;
            }

            self.add("barrier", {
                                    .target = image->label(view.range),
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
