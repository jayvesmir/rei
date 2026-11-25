#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
#include "rei/gpu/vk/buffer.hpp"
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

        inline auto label(std::string_view name, std::string_view kind) -> std::string {
            return name.empty() ? std::format("unnamed {}", kind) : std::string{name};
        }

        inline auto quoted(std::string_view name, std::string_view kind) -> std::string {
            return name.empty() ? std::format("an unnamed {}", kind) : std::format("{} '{}'", kind, name);
        }

        struct command_pool {
            VkCommandPool pool{};
            VkCommandBuffer buffer{};
        };

        struct retired {
            std::uint64_t serial{};
            std::string label;
            VkBuffer buffer{};
            VkImage image{};
            VkImageView view{};
            VkSampler sampler{};
            detail::command_pool pool{};
            VmaAllocation allocation{};
            VkDeviceMemory memory{};
        };
    } // namespace detail

    struct instance::state {
        VkInstance handle{};
        volk::VolkInstanceTable table{};
        VkDebugUtilsMessengerEXT messenger{};
        std::vector<std::string> extensions;
        std::vector<vk::physical_device> physical_devices;
        std::function<void(const vk::message&)> on_message;
        vk::validation validation{vk::validation::none};
        bool trace{};

        state() = default;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();
    };

    struct queue::state {
        VkQueue handle{};
        std::uint32_t family{};
        VkSemaphore timeline{};
        std::atomic<std::uint64_t> submitted{};
        std::mutex mutex;
    };

    struct device::state {
        vk::instance instance;
        vk::physical_device physical;
        VkPhysicalDeviceLimits limits{};
        VkDevice handle{};
        volk::VolkDeviceTable table{};
        VmaAllocator allocator{};
        std::vector<bool> features;
        bool tracing{};
        vk::queue::state queue;

        std::mutex mutex;
        std::uint64_t next_serial{1};
        std::map<std::uint64_t, std::uint64_t> unfinished;
        std::vector<detail::retired> retired;
        std::vector<detail::command_pool> free_pools;

        state(vk::instance instance, vk::physical_device physical) noexcept;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();

        auto begin_commands() -> std::uint64_t;
        auto end_commands(std::uint64_t serial, std::optional<std::uint64_t> ticket,
                          std::vector<detail::retired> items) -> void;
        auto take_pool() -> std::expected<detail::command_pool, rei::error>;

        auto retire(detail::retired item) -> void;
        auto collect() -> void;
        auto release(std::vector<detail::retired> items) -> void;

        auto name(VkObjectType type, std::uint64_t object, std::string_view name) const -> void;
        auto emit(std::string_view text) const -> void;

        template <typename... Args>
        auto trace(std::format_string<Args...> format, Args&&... args) const -> void {
            if (tracing) {
                emit(std::format(format, std::forward<Args>(args)...));
            }
        }
    };

    struct buffer::state {
        std::shared_ptr<vk::device::state> device;
        VkBuffer handle{};
        VmaAllocation allocation{};
        VkDeviceMemory memory{};
        std::uint64_t size{};
        vk::buffer_usage usage{};
        vk::host_access host{vk::host_access::none};
        std::span<std::byte> mapped;
        std::optional<std::uint64_t> address;
        std::string name;

        ~state();
    };
} // namespace rei::gpu::vk
