#pragma once

#include <algorithm>
#include <array>
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
#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/descriptor.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/image.hpp"
#include "rei/gpu/vk/instance.hpp"
#include "rei/gpu/vk/pipeline.hpp"
#include "rei/gpu/vk/shader.hpp"
#include "rei/gpu/vk/swapchain.hpp"

namespace rei::gpu::vk {
    namespace detail {
        inline auto vulkan_error(VkResult result, std::string_view call, std::string_view context = {},
                                 std::source_location location = std::source_location::current())
            -> rei::error {
            constexpr auto names = std::to_array<std::pair<VkResult, std::string_view>>({
                {VK_ERROR_OUT_OF_HOST_MEMORY, "VK_ERROR_OUT_OF_HOST_MEMORY"},
                {VK_ERROR_OUT_OF_DEVICE_MEMORY, "VK_ERROR_OUT_OF_DEVICE_MEMORY"},
                {VK_ERROR_INITIALIZATION_FAILED, "VK_ERROR_INITIALIZATION_FAILED"},
                {VK_ERROR_DEVICE_LOST, "VK_ERROR_DEVICE_LOST"},
                {VK_ERROR_MEMORY_MAP_FAILED, "VK_ERROR_MEMORY_MAP_FAILED"},
                {VK_ERROR_LAYER_NOT_PRESENT, "VK_ERROR_LAYER_NOT_PRESENT"},
                {VK_ERROR_EXTENSION_NOT_PRESENT, "VK_ERROR_EXTENSION_NOT_PRESENT"},
                {VK_ERROR_FEATURE_NOT_PRESENT, "VK_ERROR_FEATURE_NOT_PRESENT"},
                {VK_ERROR_INCOMPATIBLE_DRIVER, "VK_ERROR_INCOMPATIBLE_DRIVER"},
                {VK_ERROR_TOO_MANY_OBJECTS, "VK_ERROR_TOO_MANY_OBJECTS"},
                {VK_ERROR_FORMAT_NOT_SUPPORTED, "VK_ERROR_FORMAT_NOT_SUPPORTED"},
                {VK_ERROR_FRAGMENTED_POOL, "VK_ERROR_FRAGMENTED_POOL"},
                {VK_ERROR_UNKNOWN, "VK_ERROR_UNKNOWN"},
                {VK_ERROR_SURFACE_LOST_KHR, "VK_ERROR_SURFACE_LOST_KHR"},
                {VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR"},
                {VK_ERROR_OUT_OF_DATE_KHR, "VK_ERROR_OUT_OF_DATE_KHR"},
            });

            auto code = vk::error_code::driver_error;
            if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                code = vk::error_code::out_of_memory;
            } else if (result == VK_ERROR_DEVICE_LOST) {
                code = vk::error_code::device_lost;
            } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
                code = vk::error_code::surface_lost;
            }

            auto name = std::string_view{};
            for (const auto& [known, spelled] : names) {
                if (known == result) {
                    name = spelled;
                }
            }

            const auto text = name.empty()
                                  ? std::format("{} returned VkResult {}", call, std::to_underlying(result))
                                  : std::format("{} returned {}", call, name);

            return rei::error{code, context.empty() ? text : std::format("{}: {}", context, text), location};
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
                return std::unexpected(detail::vulkan_error(result, call_name));
            }

            return items;
        }

        inline auto fits(std::uint64_t size, std::uint64_t offset, std::uint64_t count) noexcept -> bool {
            return offset <= size && count <= size - offset;
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

        inline auto type_name(vk::descriptor_type type) noexcept -> std::string_view {
            switch (static_cast<VkDescriptorType>(type)) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                return "sampler";
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                return "combined_image_sampler";
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                return "sampled_image";
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                return "storage_image";
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                return "uniform_texel_buffer";
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                return "storage_texel_buffer";
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                return "uniform_buffer";
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                return "storage_buffer";
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                return "input_attachment";
            default:
                return "descriptor of a type rei doesn't know";
            }
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
            VkDescriptorSetLayout set_layout{};
            VkDescriptorPool descriptor_pool{};
            VkDescriptorSet descriptor_set{};
            VkPipeline pipeline{};
            VkPipelineLayout pipeline_layout{};
            detail::command_pool pool{};
            VmaAllocation allocation{};
            VkDeviceMemory memory{};
            VkSwapchainKHR swapchain{};
            std::vector<VkSemaphore> semaphores;
            std::optional<vk::surface> surface;
            std::uint64_t ticket{};
        };

        struct barrier {
            std::string target;
            VkBuffer buffer{};
            VkImage image{};
            VkImageSubresourceRange range{};
            detail::use from;
            detail::use to;
            bool discard{};
            bool folded{};
        };

        struct shader_binding {
            std::uint32_t set{};
            std::uint32_t binding{};
            vk::descriptor_type type{};
            std::uint32_t count{};
            std::string name;
        };

        struct shader_input {
            std::uint32_t location{};
            vk::format format{vk::format::undefined};
            std::string name;
        };

        struct descriptor_writes {
            std::vector<VkDescriptorBufferInfo> buffers;
            std::vector<VkDescriptorImageInfo> images;
            std::vector<VkWriteDescriptorSet> writes;
        };

        struct reference {
            std::uint32_t binding{};
            std::uint32_t element{};
            std::string target;
            std::weak_ptr<const void> resource;
        };

        struct target_formats {
            std::vector<vk::format> color;
            std::optional<vk::format> depth;
            std::uint32_t samples{1};

            auto operator==(const target_formats& other) const noexcept -> bool = default;
        };

        struct pipeline_state {
            std::string_view kind;
            VkPipeline handle{};
            VkPipelineLayout layout{};
            std::vector<vk::set_layout> sets;
            std::vector<std::vector<std::uint32_t>> reads;
            std::uint32_t push_constant_size{};
            bool writes{};
            bool adopted{};
            std::string name;
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

    struct surface::state {
        vk::instance instance;
        VkSurfaceKHR handle{};
        std::mutex mutex;
        VkDevice presenter{};
        VkSwapchainKHR live{};

        state(vk::instance instance, VkSurfaceKHR handle) noexcept;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();
    };

    struct queue::state {
        vk::device::state& device;
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
        bool labeling{};
        bool presenting{};
        vk::queue::state queue;

        std::mutex mutex;
        std::uint64_t next_serial{1};
        std::map<std::uint64_t, std::uint64_t> unfinished;
        std::vector<detail::retired> retired;
        std::vector<std::weak_ptr<vk::image::state>> new_images;
        std::vector<detail::command_pool> free_pools;

        std::mutex descriptor_mutex;
        std::vector<VkDescriptorPool> descriptor_pools;

        std::atomic<std::uint32_t> storage_writers{};

        state(vk::instance instance, vk::physical_device physical) noexcept;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();

        auto end_commands(std::uint64_t serial, std::optional<std::uint64_t> ticket,
                          std::vector<detail::retired> items) -> void;
        auto take_pool() -> std::expected<detail::command_pool, rei::error>;

        auto retire(detail::retired item) -> void;
        auto collect(std::uint64_t idle_through = 0) -> void;
        auto release(std::span<const detail::retired> items) -> void;

        auto name(VkObjectType type, std::uint64_t object, std::string_view name) const -> void;

        template <typename... Args>
        auto trace(std::format_string<Args...> format, Args&&... args) const -> void {
            if (!tracing) {
                return;
            }

            const auto text = std::format(format, std::forward<Args>(args)...);
            instance.m_state->on_message({.severity = vk::severity::trace, .id = {}, .text = text});
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

    struct image::state {
        std::shared_ptr<vk::device::state> device;
        VkImage handle{};
        VmaAllocation allocation{};
        VkDeviceMemory memory{};
        vk::format format{vk::format::undefined};
        vk::extent extent{};
        vk::image_usage usage{};
        std::uint32_t mip_levels{1};
        std::uint32_t array_layers{1};
        std::uint32_t samples{1};
        bool cube{};
        vk::access home{vk::access::manual};
        std::string name;
        std::weak_ptr<vk::swapchain::generation> swapchain;
        std::uint32_t swapchain_index{};

        ~state();

        [[nodiscard]] auto range() const noexcept -> VkImageSubresourceRange;
        [[nodiscard]] auto extent_of(std::uint32_t mip) const noexcept -> vk::extent;
        [[nodiscard]] auto label(const VkImageSubresourceRange& range) const -> std::string;
    };

    struct image_view::state {
        std::shared_ptr<vk::device::state> device;
        std::weak_ptr<vk::image::state> image;
        std::string image_name;
        VkImageView handle{};
        vk::format format{vk::format::undefined};
        vk::extent extent{};
        VkImageSubresourceRange range{};
        std::string name;
        bool own{};

        ~state();

        [[nodiscard]] auto quoted() const -> std::string;
    };

    struct sampler::state {
        std::shared_ptr<vk::device::state> device;
        VkSampler handle{};
        std::string name;

        ~state();
    };

    struct shader::state {
        std::vector<std::uint32_t> spirv;
        vk::shader_stage stage{};
        std::string entry_point;
        vk::extent local_size{};
        std::vector<detail::shader_binding> bindings;
        std::vector<detail::shader_input> inputs;
        std::uint32_t push_constant_size{};
        bool writes{};
    };

    struct set_layout::state {
        std::shared_ptr<vk::device::state> device;
        VkDescriptorSetLayout handle{};
        std::vector<vk::set_binding> bindings;
        bool pushed{};
        std::string name;

        ~state();

        [[nodiscard]] auto resolve(std::span<const vk::descriptor> descriptors, VkDescriptorSet set) const
            -> std::expected<detail::descriptor_writes, std::string>;

        static auto lay_out(detail::pipeline_state& pipeline, const vk::device::state& context,
                            std::span<const vk::shader> shaders, std::span<const vk::set_layout> sets,
                            std::uint32_t push_constant_size) -> std::expected<void, rei::error>;
    };

    struct descriptor_set::state {
        std::shared_ptr<vk::device::state> device;
        std::shared_ptr<const vk::set_layout::state> layout;
        VkDescriptorPool pool{};
        VkDescriptorSet handle{};
        std::vector<detail::reference> references;
        std::map<std::pair<std::uint32_t, std::uint32_t>, vk::sampler> samplers;
        std::string name;

        ~state();
    };

    struct compute_pipeline::state : detail::pipeline_state {
        std::shared_ptr<vk::device::state> device;

        ~state();
    };

    struct graphics_pipeline::state : detail::pipeline_state {
        std::shared_ptr<vk::device::state> device;
        detail::target_formats targets;
        std::uint32_t vertex_inputs{};

        ~state();
    };

    struct swapchain::generation {
        struct slot {
            vk::image image;
            VkSemaphore acquired{};
            std::vector<VkSemaphore> rendered;
            std::uint32_t signaled{};
            std::uint64_t acquisitions{};
            bool awaited{};
            bool fresh{true};
            bool held{};
        };

        std::shared_ptr<vk::device::state> device;
        vk::surface surface;
        VkSwapchainKHR handle{};
        vk::extent extent{};
        vk::present_mode mode{};
        std::uint32_t min_images{};
        std::string name;

        std::mutex mutex;
        std::vector<generation::slot> slots;
        std::vector<VkSemaphore> spare;
        std::uint32_t held{};
        std::uint64_t presented{};
        std::atomic<bool> stale{};

        generation(std::shared_ptr<vk::device::state> device, vk::surface surface) noexcept;
        generation(const generation& other) = delete;
        auto operator=(const generation& other) -> generation& = delete;
        ~generation();
    };

    struct swapchain::state {
        std::shared_ptr<vk::device::state> device;
        vk::surface surface;
        VkSurfaceFormatKHR format{};
        vk::image_usage usage{};
        vk::present_mode mode{};
        vk::extent requested{};
        vk::extent extent{};
        std::string name;
        std::shared_ptr<swapchain::generation> current;

        auto build(vk::extent wanted) -> std::expected<void, rei::error>;
    };

    struct frame::state {
        std::shared_ptr<vk::swapchain::generation> generation;
        std::uint32_t index{};
        bool presented{};

        ~state();
    };

    struct commands::state {
        struct frame_use {
            std::shared_ptr<vk::swapchain::generation> generation;
            std::uint32_t index{};
            std::uint64_t acquisition{};
        };

        struct bound_set {
            bool pushed{};
            std::shared_ptr<const vk::descriptor_set::state> allocated;
        };

        struct scope {
            std::string label;
            std::string workflow;
            VkPipelineBindPoint bind_point{};
            std::shared_ptr<const detail::pipeline_state> pipeline;
            std::vector<state::bound_set> sets;
            bool wrote{};
        };

        struct compute_scope : state::scope {
            bool concurrent{};
            bool last_wrote{};
        };

        struct render_target {
            std::shared_ptr<const vk::image::state> image;
            VkImageSubresourceRange range{};
            vk::access needed{};
            bool overwritten{};
            bool wrote{};
        };

        struct render_scope : state::scope {
            std::vector<state::render_target> targets;
            detail::target_formats formats;
            std::uint32_t vertex_inputs{};
            std::vector<bool> vertex_buffers;
            bool index_buffer{};
            bool waited{};
        };

        std::shared_ptr<vk::device::state> device;
        std::uint64_t serial{};
        std::string name;
        detail::command_pool pool{};
        std::vector<detail::retired> staging;
        std::vector<detail::barrier> pending;
        std::vector<state::frame_use> frames;
        std::optional<state::compute_scope> compute;
        std::optional<state::render_scope> render;
        std::optional<rei::error> failure;
        bool settled{};

        state(std::shared_ptr<vk::device::state> device, std::uint64_t serial, std::string name) noexcept;
        state(const state& other) = delete;
        auto operator=(const state& other) -> state& = delete;
        ~state();

        auto fail(std::error_code code, std::string detail) -> void;
        auto outside(std::string_view call) -> bool;
        auto inside(bool open, std::string_view call, std::string_view pass) -> bool;
        auto alive(std::string_view call, const image_view::state& view) -> std::shared_ptr<image::state>;

        template <typename StateType>
        auto usable(const std::shared_ptr<StateType>& resource, std::string_view call, std::string_view kind)
            -> bool {
            if (!resource) {
                fail(vk::error_code::invalid_argument, std::format("{}: the {} was moved from", call, kind));
            } else if (resource->device != device) {
                fail(vk::error_code::invalid_argument, std::format("{}: {} belongs to another device", call,
                                                                   detail::quoted(resource->name, kind)));
            }

            return !failure;
        }

        template <typename... Args>
        auto trace(std::string_view workflow, std::format_string<Args...> format, Args&&... args) const
            -> void {
            if (device->tracing) {
                device->trace("{}: {}: {}", detail::label(name, "commands"), workflow,
                              std::format(format, std::forward<Args>(args)...));
            }
        }

        auto begin(std::string_view workflow) const -> void;
        auto end() const -> void;

        auto add(std::string_view workflow, detail::barrier barrier) -> void;
        auto flush(std::string_view workflow) -> void;
        auto touch(const image::state& image) -> void;

        auto enter(std::string_view workflow, const image::state& image, const VkImageSubresourceRange& range,
                   vk::access needed, bool discard) -> void;
        auto leave(std::string_view workflow, const image::state& image, const VkImageSubresourceRange& range,
                   vk::access needed, bool wrote) -> void;
        auto enter(std::string_view workflow, const buffer::state& buffer) -> void;
        auto leave(std::string_view workflow, const buffer::state& buffer) -> void;

        auto stage(std::string_view call, std::string_view workflow, std::span<const std::byte> data)
            -> std::optional<VkBuffer>;

        auto bind(state::scope& scope, std::uint32_t set,
                  const std::shared_ptr<vk::descriptor_set::state>& descriptors) -> void;
        auto push_descriptors(state::scope& scope, std::uint32_t set,
                              std::span<const vk::descriptor> descriptors) -> void;
        auto push_constants(const state::scope& scope, std::span<const std::byte> data) -> void;
        auto readable(const state::scope& scope, std::string_view call) -> bool;
    };
} // namespace rei::gpu::vk
