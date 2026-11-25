#pragma once

#include <cstdint>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/format.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk::detail {
    struct format_info {
        std::string_view name;
        std::uint32_t block_size;
        vk::extent block;
        VkImageAspectFlags aspects;
        bool planar;
    };

    [[nodiscard]] auto info_of(vk::format format) noexcept -> detail::format_info;
} // namespace rei::gpu::vk::detail
