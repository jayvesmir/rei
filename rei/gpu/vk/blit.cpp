#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/formats.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    auto commands::blit(const vk::image_view& source, const vk::image_view& destination, vk::filter filter)
        -> void {
        auto& self = *m_state;

        if (!self.outside("blit from an image view") ||
            !self.usable(source.m_state, "blit from an image view", "image view") ||
            !self.usable(destination.m_state, "blit into an image view", "image view")) {
            return;
        }

        const auto& from = *source.m_state;
        const auto& into = *destination.m_state;
        const auto call = [&] -> std::string {
            return std::format("blit from {} into {}", from.quoted(), into.quoted());
        };
        const auto origin = self.alive(call, from);
        const auto target = self.alive(call, into);
        if (!origin || !target) {
            return;
        }

        const auto origin_format = detail::info_of(origin->format);
        const auto target_format = detail::info_of(target->format);
        const auto& physical = self.device->physical;

        if (!vk::has(origin->usage, vk::image_usage::transfer_src) ||
            !vk::has(target->usage, vk::image_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the source needs the transfer_src usage, the destination transfer_dst",
                                  call()));
            return;
        }
        if (origin->samples != 1 || target->samples != 1) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("{}: a multisampled image is resolved by a render pass, not blitted", call()));
            return;
        }
        if (from.range.layerCount != into.range.layerCount) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the source has {} array layers and the destination {}", call(),
                                  from.range.layerCount, into.range.layerCount));
            return;
        }
        if (origin == target && from.range.baseMipLevel == into.range.baseMipLevel &&
            from.range.baseArrayLayer < into.range.baseArrayLayer + into.range.layerCount &&
            into.range.baseArrayLayer < from.range.baseArrayLayer + from.range.layerCount) {
            self.fail(vk::error_code::invalid_argument, std::format("{}: the two overlap", call()));
            return;
        }

        auto origin_support = VkFormatProperties{};
        auto target_support = VkFormatProperties{};
        self.device->instance.raw().vkGetPhysicalDeviceFormatProperties(
            physical.handle, static_cast<VkFormat>(origin->format), &origin_support);
        self.device->instance.raw().vkGetPhysicalDeviceFormatProperties(
            physical.handle, static_cast<VkFormat>(target->format), &target_support);

        const auto filterable =
            filter == vk::filter::nearest ||
            (origin_support.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        const auto colors = origin_format.aspects == VK_IMAGE_ASPECT_COLOR_BIT &&
                            target_format.aspects == VK_IMAGE_ASPECT_COLOR_BIT;
        if ((origin_support.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0 ||
            (target_support.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0 || !filterable ||
            origin_format.numeric != target_format.numeric ||
            (!colors && (origin->format != target->format || filter != vk::filter::nearest))) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: '{}' can't blit {} into {} with the {} filter", call(), physical.name,
                                  origin_format.name, target_format.name,
                                  filter == vk::filter::nearest ? "nearest" : "linear"));
            return;
        }

        const auto origin_range = VkImageSubresourceRange{
            .aspectMask = from.range.aspectMask,
            .baseMipLevel = from.range.baseMipLevel,
            .levelCount = 1,
            .baseArrayLayer = from.range.baseArrayLayer,
            .layerCount = from.range.layerCount,
        };
        const auto target_range = VkImageSubresourceRange{
            .aspectMask = into.range.aspectMask,
            .baseMipLevel = into.range.baseMipLevel,
            .levelCount = 1,
            .baseArrayLayer = into.range.baseArrayLayer,
            .layerCount = into.range.layerCount,
        };
        const auto workflow =
            std::format("blit {} → {}", origin->label(origin_range), target->label(target_range));
        self.begin(workflow);
        self.enter(workflow, *origin, origin_range, vk::access::transfer_src, false);
        self.enter(workflow, *target, target_range, vk::access::transfer_dst, true);
        self.flush(workflow);

        const auto region = VkImageBlit{
            .srcSubresource =
                {
                    .aspectMask = origin_range.aspectMask,
                    .mipLevel = origin_range.baseMipLevel,
                    .baseArrayLayer = origin_range.baseArrayLayer,
                    .layerCount = origin_range.layerCount,
                },
            .srcOffsets =
                {
                    {},
                    {
                        .x = static_cast<std::int32_t>(from.extent.width),
                        .y = static_cast<std::int32_t>(from.extent.height),
                        .z = static_cast<std::int32_t>(from.extent.depth),
                    },
                },
            .dstSubresource =
                {
                    .aspectMask = target_range.aspectMask,
                    .mipLevel = target_range.baseMipLevel,
                    .baseArrayLayer = target_range.baseArrayLayer,
                    .layerCount = target_range.layerCount,
                },
            .dstOffsets =
                {
                    {},
                    {
                        .x = static_cast<std::int32_t>(into.extent.width),
                        .y = static_cast<std::int32_t>(into.extent.height),
                        .z = static_cast<std::int32_t>(into.extent.depth),
                    },
                },
        };
        self.device->table.vkCmdBlitImage(
            self.pool.buffer, origin->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->handle,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, static_cast<VkFilter>(filter));

        self.leave(workflow, *origin, origin_range, vk::access::transfer_src, false);
        self.leave(workflow, *target, target_range, vk::access::transfer_dst, true);
        self.end();
    }

    auto commands::generate_mipmaps(const vk::image& image) -> void {
        auto& self = *m_state;

        if (!self.outside("generate_mipmaps") || !self.usable(image.m_state, "generate_mipmaps", "image")) {
            return;
        }

        const auto& target = *image.m_state;
        const auto call = std::format("generate_mipmaps of {}", detail::quoted(target.name, "image"));
        const auto format = detail::info_of(target.format);
        const auto& physical = self.device->physical;

        if (!vk::has(target.usage, vk::image_usage::transfer_src) ||
            !vk::has(target.usage, vk::image_usage::transfer_dst)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: it needs the transfer_src and transfer_dst usages", call));
            return;
        }
        if (target.home == vk::access::manual) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: its home is manual, so its mips are yours to blit", call));
            return;
        }

        constexpr auto blits =
            VkFormatFeatureFlags{VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT};
        constexpr auto needed =
            VkFormatFeatureFlags{blits | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT};
        auto support = VkFormatProperties{};
        self.device->instance.raw().vkGetPhysicalDeviceFormatProperties(
            physical.handle, static_cast<VkFormat>(target.format), &support);
        if (target.samples != 1 || format.aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
            (support.optimalTilingFeatures & needed) != needed) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: '{}' can't blit {} images of {} samples with the linear filter", call,
                                  physical.name, format.name, target.samples));
            return;
        }
        if (target.mip_levels == 1) {
            return;
        }

        const auto levels = [&](std::uint32_t first, std::uint32_t count) -> VkImageSubresourceRange {
            return {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = first,
                .levelCount = count,
                .baseArrayLayer = 0,
                .layerCount = target.array_layers,
            };
        };
        const auto workflow = std::format("generate_mipmaps {}", detail::label(target.name, "image"));
        self.begin(workflow);
        self.enter(workflow, target, levels(0, 1), vk::access::transfer_src, false);
        self.enter(workflow, target, levels(1, target.mip_levels - 1), vk::access::transfer_dst, true);

        for (auto mip = 1U; mip < target.mip_levels; mip++) {
            self.flush(workflow);

            const auto larger = target.extent_of(mip - 1);
            const auto smaller = target.extent_of(mip);
            const auto region = VkImageBlit{
                .srcSubresource =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = mip - 1,
                        .baseArrayLayer = 0,
                        .layerCount = target.array_layers,
                    },
                .srcOffsets =
                    {
                        {},
                        {
                            .x = static_cast<std::int32_t>(larger.width),
                            .y = static_cast<std::int32_t>(larger.height),
                            .z = static_cast<std::int32_t>(larger.depth),
                        },
                    },
                .dstSubresource =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = mip,
                        .baseArrayLayer = 0,
                        .layerCount = target.array_layers,
                    },
                .dstOffsets =
                    {
                        {},
                        {
                            .x = static_cast<std::int32_t>(smaller.width),
                            .y = static_cast<std::int32_t>(smaller.height),
                            .z = static_cast<std::int32_t>(smaller.depth),
                        },
                    },
            };
            self.device->table.vkCmdBlitImage(
                self.pool.buffer, target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.handle,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
            self.trace(workflow, "blit mip {} → {}", mip - 1, mip);

            if (mip + 1 < target.mip_levels) {
                self.add(workflow, {
                                       .target = target.label(levels(mip, 1)),
                                       .image = target.handle,
                                       .range = levels(mip, 1),
                                       .from = detail::use_of(vk::access::transfer_dst),
                                       .to = detail::use_of(vk::access::transfer_src),
                                   });
            }
        }

        self.leave(workflow, target, levels(0, target.mip_levels - 1), vk::access::transfer_src, false);
        self.leave(workflow, target, levels(target.mip_levels - 1, 1), vk::access::transfer_dst, true);
        self.end();
    }
} // namespace rei::gpu::vk
