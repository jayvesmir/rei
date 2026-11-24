#include <mutex>

#undef VOLK_NO_DEVICE_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk.h"

#include "rei/gpu/vk/detail/loader.hpp"

namespace rei::gpu::vk::detail {
    namespace {
        std::mutex tables;
    } // namespace

    auto load_loader() -> bool {
        static const auto loaded = volk::volkInitialize() == VK_SUCCESS;
        return loaded;
    }

    auto load(volk::VolkInstanceTable& table, VkInstance instance) -> void {
        const auto lock = std::scoped_lock{tables};
        volk::volkLoadInstanceTable(&table, instance);
    }

    auto load(volk::VolkDeviceTable& table, VkDevice device) -> void {
        const auto lock = std::scoped_lock{tables};
        volk::volkLoadDeviceTable(&table, device);
    }
} // namespace rei::gpu::vk::detail
