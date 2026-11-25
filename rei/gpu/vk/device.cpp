#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
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
#include "rei/gpu/vk/detail/features.hpp"
#include "rei/gpu/vk/detail/loader.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    namespace {
        constexpr auto always_enabled = std::array{
            vk::feature::timeline_semaphore, vk::feature::synchronization2, vk::feature::dynamic_rendering,
            vk::feature::push_descriptor,    vk::feature::maintenance5,     vk::feature::scalar_block_layout,
        };

        struct candidate {
            vk::physical_device physical;
            VkPhysicalDeviceLimits limits;
            std::uint32_t queue_family;
            std::vector<bool> features;
            std::vector<std::string> extensions;
        };

        auto rank(vk::device_type type) noexcept -> std::int32_t {
            switch (type) {
            case vk::device_type::discrete_gpu:
                return 0;
            case vk::device_type::integrated_gpu:
                return 1;
            case vk::device_type::virtual_gpu:
                return 2;
            case vk::device_type::cpu:
                return 3;
            case vk::device_type::other:
                return 4;
            }

            return 4;
        }

        auto assess(const volk::VolkInstanceTable& table, std::span<const std::string> instance_extensions,
                    const vk::physical_device& physical, const vk::device_options& options)
            -> std::expected<candidate, rei::error> {
            auto properties = VkPhysicalDeviceProperties{};
            table.vkGetPhysicalDeviceProperties(physical.handle, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_4) {
                return detail::failure(vk::error_code::no_suitable_device,
                                       std::format("'{}' supports Vulkan {}.{} and rei needs 1.4",
                                                   physical.name, VK_API_VERSION_MAJOR(properties.apiVersion),
                                                   VK_API_VERSION_MINOR(properties.apiVersion)));
            }

            auto family_count = std::uint32_t{};
            table.vkGetPhysicalDeviceQueueFamilyProperties(physical.handle, &family_count, nullptr);
            auto families = std::vector<VkQueueFamilyProperties>(family_count);
            table.vkGetPhysicalDeviceQueueFamilyProperties(physical.handle, &family_count, families.data());

            const auto family =
                std::ranges::find_if(families, [](const VkQueueFamilyProperties& properties) -> bool {
                    constexpr auto needed = VkQueueFlags{VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT};
                    return (properties.queueFlags & needed) == needed;
                });
            if (family == families.end()) {
                return detail::failure(
                    vk::error_code::no_suitable_device,
                    std::format("'{}' has no queue for graphics and compute", physical.name));
            }

            const auto supported = detail::enumerate<VkExtensionProperties>(
                "vkEnumerateDeviceExtensionProperties",
                [&](std::uint32_t* count, VkExtensionProperties* items) -> VkResult {
                    return table.vkEnumerateDeviceExtensionProperties(physical.handle, nullptr, count, items);
                });
            if (!supported) {
                return std::unexpected(supported.error());
            }

            const auto available = [&](const detail::extension_requirement& requirement) -> bool {
                return requirement.is_instance ? std::ranges::contains(instance_extensions, requirement.name)
                                               : detail::has_extension(*supported, requirement.name);
            };

            auto wanted = std::ranges::to<std::vector>(always_enabled);
            wanted.append_range(options.require);
            const auto required = wanted.size();
            wanted.append_range(options.prefer);

            auto query = detail::feature_chain{};
            for (const auto feature : wanted) {
                if (std::ranges::all_of(detail::info_of(feature).extensions, available)) {
                    query.add(detail::info_of(feature));
                }
            }
            table.vkGetPhysicalDeviceFeatures2(physical.handle, &query.root());

            auto chosen = candidate{
                .physical = physical,
                .limits = properties.limits,
                .queue_family = static_cast<std::uint32_t>(std::distance(families.begin(), family)),
                .features = std::vector<bool>(detail::feature_table.size()),
                .extensions = {},
            };
            const auto enable = [&](std::string_view extension) -> void {
                if (!std::ranges::contains(chosen.extensions, extension)) {
                    chosen.extensions.emplace_back(extension);
                }
            };

            for (auto i = 0uz; i < wanted.size(); i++) {
                const auto& info = detail::info_of(wanted[i]);
                const auto missing = std::ranges::find_if_not(info.extensions, available);

                if (missing == info.extensions.end() && query.has(info)) {
                    chosen.features[std::to_underlying(wanted[i])] = true;
                    for (const auto& requirement : info.extensions) {
                        if (!requirement.is_instance) {
                            enable(requirement.name);
                        }
                    }
                } else if (i < required && missing == info.extensions.end()) {
                    return detail::failure(vk::error_code::missing_feature,
                                           std::format("'{}' lacks {}", physical.name, info.name));
                } else if (i < required) {
                    return detail::failure(
                        vk::error_code::missing_feature,
                        std::format("'{}' lacks {}, which needs the {} extension {}", physical.name,
                                    info.name, missing->is_instance ? "instance" : "device", missing->name));
                }
            }

            for (const auto& name : options.extensions) {
                if (!detail::has_extension(*supported, name)) {
                    return detail::failure(vk::error_code::missing_extension,
                                           std::format("'{}' lacks the extension {}", physical.name, name));
                }
                enable(name);
            }

            return chosen;
        }

        auto choose(const volk::VolkInstanceTable& table, std::span<const std::string> instance_extensions,
                    std::span<const vk::physical_device> physical_devices, const vk::device_options& options)
            -> std::expected<candidate, rei::error> {
            if (options.physical) {
                return assess(table, instance_extensions, *options.physical, options);
            }

            auto best = std::optional<candidate>{};
            auto reasons = std::vector<std::string>{};

            for (const auto& physical : physical_devices) {
                auto assessed = assess(table, instance_extensions, physical, options);

                if (!assessed) {
                    reasons.push_back(assessed.error().detail());
                } else if (!best || rank(physical.type) < rank(best->physical.type)) {
                    best = std::move(*assessed);
                }
            }

            if (!best) {
                const auto joined =
                    reasons | std::views::join_with(std::string_view{"; "}) | std::ranges::to<std::string>();
                return detail::failure(vk::error_code::no_suitable_device,
                                       reasons.empty() ? "no Vulkan device was found"
                                                       : "no suitable device: " + joined);
            }

            return std::move(*best);
        }
    } // namespace

    device::state::state(vk::instance instance, vk::physical_device physical) noexcept
        : instance(std::move(instance)), physical(std::move(physical)) {}

    device::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        table.vkDeviceWaitIdle(handle);
        vmaDestroyAllocator(allocator);
        table.vkDestroyDevice(handle, nullptr);
    }

    auto device::state::emit(std::string_view text) const -> void {
        instance.m_state->on_message({.severity = vk::severity::trace, .id = {}, .text = text});
    }

    device::device(std::shared_ptr<device::state> state) noexcept : m_state(std::move(state)) {}

    auto device::make(const vk::instance& instance, const vk::device_options& options)
        -> std::expected<vk::device, rei::error> {
        const auto& context = *instance.m_state;

        auto chosen = choose(context.table, context.extensions, context.physical_devices, options);
        if (!chosen) {
            return std::unexpected(chosen.error());
        }

        auto enabled = detail::feature_chain{};
        for (const auto [selected, info] : std::views::zip(chosen->features, detail::feature_table)) {
            if (selected) {
                enabled.enable(info);
            }
        }

        const auto priority = 1.0f;
        const auto queue = VkDeviceQueueCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = chosen->queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        const auto extension_names =
            chosen->extensions |
            std::views::transform([](const std::string& name) -> const char* { return name.c_str(); }) |
            std::ranges::to<std::vector>();
        const auto info = VkDeviceCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &enabled.root(),
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount = static_cast<std::uint32_t>(extension_names.size()),
            .ppEnabledExtensionNames = extension_names.data(),
        };

        auto state = std::make_shared<device::state>(instance, chosen->physical);
        if (const auto result =
                context.table.vkCreateDevice(chosen->physical.handle, &info, nullptr, &state->handle);
            result != VK_SUCCESS) {
            return detail::failure(result, "vkCreateDevice");
        }

        detail::load(state->table, state->handle);
        state->table.vkGetDeviceQueue(state->handle, chosen->queue_family, 0, &state->queue.handle);
        state->queue.family = chosen->queue_family;
        state->limits = chosen->limits;
        state->features = std::move(chosen->features);
        state->tracing = context.trace;

        const auto functions = VmaVulkanFunctions{
            .vkGetInstanceProcAddr = volk::vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = context.table.vkGetDeviceProcAddr,
        };
        const auto allocator = VmaAllocatorCreateInfo{
            .physicalDevice = chosen->physical.handle,
            .device = state->handle,
            .pVulkanFunctions = &functions,
            .instance = context.handle,
            .vulkanApiVersion = VK_API_VERSION_1_4,
        };
        if (const auto result = vmaCreateAllocator(&allocator, &state->allocator); result != VK_SUCCESS) {
            return detail::failure(result, "vmaCreateAllocator");
        }

        if (!options.name.empty() && context.table.vkSetDebugUtilsObjectNameEXT != nullptr) {
            const auto name = VkDebugUtilsObjectNameInfoEXT{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .objectType = VK_OBJECT_TYPE_DEVICE,
                .objectHandle = reinterpret_cast<std::uint64_t>(state->handle),
                .pObjectName = options.name.c_str(),
            };
            context.table.vkSetDebugUtilsObjectNameEXT(state->handle, &name);
        }

        return vk::device{std::move(state)};
    }

    auto device::queue() const noexcept -> vk::queue {
        return vk::queue{std::shared_ptr<queue::state>{m_state, &m_state->queue}};
    }

    auto device::has(vk::feature feature) const noexcept -> bool {
        return m_state->features[std::to_underlying(feature)];
    }

    auto device::limits() const noexcept -> const VkPhysicalDeviceLimits& {
        return m_state->limits;
    }

    auto device::physical() const noexcept -> const vk::physical_device& {
        return m_state->physical;
    }

    auto device::handle() const noexcept -> VkDevice {
        return m_state->handle;
    }

    auto device::raw() const noexcept -> const volk::VolkDeviceTable& {
        return m_state->table;
    }

    queue::queue(std::shared_ptr<queue::state> state) noexcept : m_state(std::move(state)) {}

    auto queue::family() const noexcept -> std::uint32_t {
        return m_state->family;
    }

    auto queue::handle() const noexcept -> VkQueue {
        return m_state->handle;
    }
} // namespace rei::gpu::vk
