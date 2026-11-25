#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "volk/volk.h"

#include <vk_mem_alloc.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/instance.hpp"

namespace rei::gpu::vk {
    namespace detail {
        inline auto failure(std::error_code code, std::string detail,
                            std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            return std::unexpected(rei::error{code, std::move(detail), location});
        }

        inline auto result_name(VkResult result) -> std::string {
            switch (result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED:
                return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST:
                return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_MEMORY_MAP_FAILED:
                return "VK_ERROR_MEMORY_MAP_FAILED";
            case VK_ERROR_LAYER_NOT_PRESENT:
                return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT:
                return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT:
                return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER:
                return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_TOO_MANY_OBJECTS:
                return "VK_ERROR_TOO_MANY_OBJECTS";
            case VK_ERROR_FORMAT_NOT_SUPPORTED:
                return "VK_ERROR_FORMAT_NOT_SUPPORTED";
            case VK_ERROR_FRAGMENTED_POOL:
                return "VK_ERROR_FRAGMENTED_POOL";
            case VK_ERROR_UNKNOWN:
                return "VK_ERROR_UNKNOWN";
            default:
                return std::format("VkResult {}", std::to_underlying(result));
            }
        }

        inline auto failure(VkResult result, std::string_view call,
                            std::source_location location = std::source_location::current())
            -> std::unexpected<rei::error> {
            auto code = vk::error_code::driver_error;
            if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                code = vk::error_code::out_of_memory;
            } else if (result == VK_ERROR_DEVICE_LOST) {
                code = vk::error_code::device_lost;
            }

            return detail::failure(code, std::format("{} returned {}", call, detail::result_name(result)),
                                   location);
        }

        template <typename ItemType, typename CallType>
        auto enumerate(std::string_view call_name, const CallType& call)
            -> std::expected<std::vector<ItemType>, rei::error> {
            auto items = std::vector<ItemType>{};
            auto result = VK_INCOMPLETE;

            while (result == VK_INCOMPLETE) {
                auto count = std::uint32_t{};
                result = call(&count, nullptr);
                if (result != VK_SUCCESS) {
                    break;
                }

                items.resize(count);
                result = call(&count, items.data());
                items.resize(count);
            }

            if (result != VK_SUCCESS) {
                return detail::failure(result, call_name);
            }

            return items;
        }

        inline auto has_extension(std::span<const VkExtensionProperties> extensions, std::string_view name)
            -> bool {
            return std::ranges::contains(extensions, name,
                                         [](const VkExtensionProperties& extension) -> std::string_view {
                                             return extension.extensionName;
                                         });
        }
    } // namespace detail

    struct instance::state {
        VkInstance handle{};
        volk::VolkInstanceTable table{};
        VkDebugUtilsMessengerEXT messenger{};
        std::vector<std::string> extensions;
        std::vector<vk::physical_device> physical_devices;
        std::function<void(const vk::message&)> on_message;

        state() = default;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();
    };

    struct queue::state {
        VkQueue handle{};
        std::uint32_t family{};
    };

    struct device::state {
        vk::instance instance;
        vk::physical_device physical;
        VkPhysicalDeviceLimits limits{};
        VkDevice handle{};
        volk::VolkDeviceTable table{};
        VmaAllocator allocator{};
        std::vector<bool> features;
        vk::queue::state queue;

        state(vk::instance instance, vk::physical_device physical) noexcept;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();
    };
} // namespace rei::gpu::vk
