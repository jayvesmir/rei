#pragma once

#include <vulkan/vulkan_core.h>

#include "volk/volk.h"

namespace rei::gpu::vk::detail {
    auto load(volk::VolkInstanceTable& table, VkInstance instance) -> void;
    auto load(volk::VolkDeviceTable& table, VkDevice device) -> void;
} // namespace rei::gpu::vk::detail
