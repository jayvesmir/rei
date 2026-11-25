#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
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
        release(std::exchange(retired, {}));
        for (const auto& pool : free_pools) {
            table.vkDestroyCommandPool(handle, pool.pool, nullptr);
        }

        table.vkDestroySemaphore(handle, queue.timeline, nullptr);
        vmaDestroyAllocator(allocator);
        table.vkDestroyDevice(handle, nullptr);
    }

    auto device::state::begin_commands() -> std::uint64_t {
        const auto lock = std::scoped_lock{mutex};
        const auto serial = next_serial++;

        unfinished.emplace(serial, 0);
        return serial;
    }

    auto device::state::end_commands(std::uint64_t serial, std::optional<std::uint64_t> ticket,
                                     std::vector<detail::retired> items) -> void {
        const auto lock = std::scoped_lock{mutex};

        if (ticket) {
            unfinished[serial] = *ticket;
        } else {
            unfinished.erase(serial);
        }

        for (auto& item : items) {
            item.serial = serial;
            retired.push_back(std::move(item));
        }
    }

    auto device::state::take_pool() -> std::expected<detail::command_pool, rei::error> {
        {
            const auto lock = std::scoped_lock{mutex};
            if (!free_pools.empty()) {
                const auto pool = free_pools.back();
                free_pools.pop_back();
                return pool;
            }
        }

        auto pool = detail::command_pool{};
        const auto pool_info = VkCommandPoolCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = queue.family,
        };
        if (const auto result = table.vkCreateCommandPool(handle, &pool_info, nullptr, &pool.pool);
            result != VK_SUCCESS) {
            return detail::failure(result, "vkCreateCommandPool");
        }

        const auto buffer_info = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool.pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (const auto result = table.vkAllocateCommandBuffers(handle, &buffer_info, &pool.buffer);
            result != VK_SUCCESS) {
            table.vkDestroyCommandPool(handle, pool.pool, nullptr);
            return detail::failure(result, "vkAllocateCommandBuffers");
        }

        return pool;
    }

    auto device::state::retire(detail::retired item) -> void {
        const auto lock = std::scoped_lock{mutex};

        item.serial = next_serial - 1;
        retired.push_back(std::move(item));
    }

    auto device::state::collect() -> void {
        auto completed = std::uint64_t{};
        if (table.vkGetSemaphoreCounterValue(handle, queue.timeline, &completed) != VK_SUCCESS) {
            return;
        }

        auto freeable = std::vector<detail::retired>{};
        {
            const auto lock = std::scoped_lock{mutex};
            std::erase_if(unfinished, [&](const auto& entry) -> bool {
                return entry.second != 0 && entry.second <= completed;
            });

            const auto oldest = unfinished.empty() ? next_serial : unfinished.begin()->first;
            auto kept = std::vector<detail::retired>{};
            for (auto& item : retired) {
                (item.serial < oldest ? freeable : kept).push_back(std::move(item));
            }
            retired = std::move(kept);
        }

        if (freeable.empty()) {
            return;
        }

        if (tracing) {
            const auto freed =
                freeable |
                std::views::filter([](const detail::retired& item) -> bool { return !item.label.empty(); }) |
                std::views::transform(&detail::retired::label) |
                std::views::join_with(std::string_view{", "}) | std::ranges::to<std::string>();
            if (!freed.empty()) {
                trace("collect: freed {}", freed);
            }
        }

        release(std::move(freeable));
    }

    auto device::state::release(std::vector<detail::retired> items) -> void {
        for (const auto& item : items) {
            if (item.view != VK_NULL_HANDLE) {
                table.vkDestroyImageView(handle, item.view, nullptr);
            }
            if (item.sampler != VK_NULL_HANDLE) {
                table.vkDestroySampler(handle, item.sampler, nullptr);
            }

            if (item.allocation != VK_NULL_HANDLE && item.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, item.buffer, item.allocation);
            } else if (item.buffer != VK_NULL_HANDLE) {
                table.vkDestroyBuffer(handle, item.buffer, nullptr);
            }

            if (item.allocation != VK_NULL_HANDLE && item.image != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator, item.image, item.allocation);
            } else if (item.image != VK_NULL_HANDLE) {
                table.vkDestroyImage(handle, item.image, nullptr);
            }

            if (item.memory != VK_NULL_HANDLE) {
                table.vkFreeMemory(handle, item.memory, nullptr);
            }

            if (item.pool.pool != VK_NULL_HANDLE) {
                table.vkResetCommandPool(handle, item.pool.pool, 0);

                const auto lock = std::scoped_lock{mutex};
                free_pools.push_back(item.pool);
            }
        }
    }

    auto device::state::name(VkObjectType type, std::uint64_t object, std::string_view name) const -> void {
        const auto& set_name = instance.raw().vkSetDebugUtilsObjectNameEXT;
        if (name.empty() || set_name == nullptr) {
            return;
        }

        const auto text = std::string{name};
        const auto info = VkDebugUtilsObjectNameInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType = type,
            .objectHandle = object,
            .pObjectName = text.c_str(),
        };
        set_name(handle, &info);
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

        const auto timeline_type = VkSemaphoreTypeCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        const auto timeline = VkSemaphoreCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timeline_type,
        };
        if (const auto result =
                state->table.vkCreateSemaphore(state->handle, &timeline, nullptr, &state->queue.timeline);
            result != VK_SUCCESS) {
            return detail::failure(result, "vkCreateSemaphore");
        }

        const auto functions = VmaVulkanFunctions{
            .vkGetInstanceProcAddr = volk::vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = context.table.vkGetDeviceProcAddr,
        };
        const auto allocator = VmaAllocatorCreateInfo{
            .flags = state->features[std::to_underlying(vk::feature::buffer_device_address)]
                         ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT
                         : VmaAllocatorCreateFlags{},
            .physicalDevice = chosen->physical.handle,
            .device = state->handle,
            .pVulkanFunctions = &functions,
            .instance = context.handle,
            .vulkanApiVersion = VK_API_VERSION_1_4,
        };
        if (const auto result = vmaCreateAllocator(&allocator, &state->allocator); result != VK_SUCCESS) {
            return detail::failure(result, "vmaCreateAllocator");
        }

        state->name(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<std::uint64_t>(state->handle), options.name);
        state->name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(state->queue.timeline),
                    "rei timeline");

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

    auto device::wait(vk::ticket ticket, std::optional<std::chrono::nanoseconds> timeout) const
        -> std::expected<void, rei::error> {
        auto& state = *m_state;

        if (ticket.queue != 0 || ticket.value > state.queue.submitted) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("wait for ticket {}, which was never issued", ticket.value));
        }

        const auto info = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &state.queue.timeline,
            .pValues = &ticket.value,
        };
        const auto nanoseconds = timeout
                                     ? static_cast<std::uint64_t>(std::max(timeout->count(), std::int64_t{}))
                                     : std::numeric_limits<std::uint64_t>::max();

        const auto result = state.table.vkWaitSemaphores(state.handle, &info, nanoseconds);
        if (result == VK_TIMEOUT) {
            return detail::failure(vk::error_code::timed_out,
                                   std::format("ticket {} is not done after {}", ticket.value,
                                               std::chrono::duration<double, std::milli>{*timeout}));
        }
        if (result != VK_SUCCESS) {
            return detail::failure(result, "vkWaitSemaphores");
        }

        state.collect();
        return {};
    }

    auto device::wait_idle() const -> std::expected<void, rei::error> {
        auto& state = *m_state;

        auto lock = std::unique_lock{state.queue.mutex};
        const auto result = state.table.vkDeviceWaitIdle(state.handle);
        lock.unlock();

        if (result != VK_SUCCESS) {
            return detail::failure(result, "vkDeviceWaitIdle");
        }

        state.collect();
        return {};
    }

    auto device::done(vk::ticket ticket) const noexcept -> bool {
        const auto& state = *m_state;

        auto completed = std::uint64_t{};
        return ticket.queue == 0 &&
               state.table.vkGetSemaphoreCounterValue(state.handle, state.queue.timeline, &completed) ==
                   VK_SUCCESS &&
               completed >= ticket.value;
    }

    auto device::collect() const -> void {
        m_state->collect();
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
