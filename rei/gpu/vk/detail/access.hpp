#pragma once

#include <string_view>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk::detail {
    struct use {
        std::string_view name;
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
        VkAccessFlags2 access{VK_ACCESS_2_NONE};

        auto operator==(const use& other) const noexcept -> bool = default;
    };

    inline constexpr VkPipelineStageFlags2 shader_stages = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
                                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    inline constexpr VkPipelineStageFlags2 fragment_tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

    [[nodiscard]] constexpr auto use_of(vk::access access) noexcept -> detail::use {
        switch (access) {
        case vk::access::undefined:
            return {.name = "undefined"};
        case vk::access::shader_read:
            return {
                .name = "shader_read",
                .layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                .stages = detail::shader_stages,
                .access = VK_ACCESS_2_SHADER_READ_BIT,
            };
        case vk::access::shader_write:
            return {
                .name = "shader_write",
                .layout = VK_IMAGE_LAYOUT_GENERAL,
                .stages = detail::shader_stages,
                .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            };
        case vk::access::color_attachment:
            return {
                .name = "color_attachment",
                .layout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            };
        case vk::access::depth_attachment:
            return {
                .name = "depth_attachment",
                .layout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .stages = detail::fragment_tests,
                .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            };
        case vk::access::depth_read:
            return {
                .name = "depth_read",
                .layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                .stages = detail::fragment_tests | detail::shader_stages,
                .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            };
        case vk::access::transfer_src:
            return {
                .name = "transfer_src",
                .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .access = VK_ACCESS_2_TRANSFER_READ_BIT,
            };
        case vk::access::transfer_dst:
            return {
                .name = "transfer_dst",
                .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .stages = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            };
        case vk::access::present:
            return {
                .name = "present",
                .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            };
        case vk::access::manual:
            return {.name = "manual"};
        }

        return {.name = "unknown"};
    }

    [[nodiscard]] constexpr auto home_of(vk::buffer_usage usage, vk::host_access host) noexcept
        -> detail::use {
        auto home = detail::use{.name = "home"};

        if (vk::has(usage, vk::buffer_usage::transfer_src)) {
            home.stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            home.access |= VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        if (vk::has(usage, vk::buffer_usage::transfer_dst)) {
            home.stages |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            home.access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        if (vk::has(usage, vk::buffer_usage::uniform_buffer)) {
            home.stages |= detail::shader_stages;
            home.access |= VK_ACCESS_2_UNIFORM_READ_BIT;
        }
        if (vk::has(usage, vk::buffer_usage::storage_buffer) ||
            vk::has(usage, vk::buffer_usage::shader_device_address)) {
            home.stages |= detail::shader_stages;
            home.access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        }
        if (vk::has(usage, vk::buffer_usage::index_buffer)) {
            home.stages |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            home.access |= VK_ACCESS_2_INDEX_READ_BIT;
        }
        if (vk::has(usage, vk::buffer_usage::vertex_buffer)) {
            home.stages |= VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
            home.access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        }
        if (vk::has(usage, vk::buffer_usage::indirect_buffer)) {
            home.stages |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            home.access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        if (host == vk::host_access::read_write) {
            home.stages |= VK_PIPELINE_STAGE_2_HOST_BIT;
            home.access |= VK_ACCESS_2_HOST_READ_BIT;
        }

        return home;
    }
} // namespace rei::gpu::vk::detail
