#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/descriptor.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    set_layout::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        device->retire({.label = detail::label(name, "set layout"), .set_layout = handle});
    }

    auto set_layout::state::resolve(std::span<const vk::descriptor> descriptors, VkDescriptorSet set) const
        -> std::expected<detail::descriptor_writes, std::string> {
        auto resolved = detail::descriptor_writes{};
        resolved.buffers.reserve(descriptors.size());
        resolved.images.reserve(descriptors.size());

        for (const auto& descriptor : descriptors) {
            const auto declared =
                std::ranges::find(bindings, descriptor.m_binding, &vk::set_binding::binding);
            if (declared == bindings.end()) {
                return std::unexpected(std::format("binding {} isn't in {}", descriptor.m_binding,
                                                   detail::quoted(name, "set layout")));
            }
            if (descriptor.m_element >= declared->count) {
                return std::unexpected(std::format("binding {} has {} elements, and element {} was given",
                                                   descriptor.m_binding, declared->count,
                                                   descriptor.m_element));
            }

            const auto type = declared->type;
            const auto takes_buffer =
                type == vk::descriptor_type::uniform_buffer || type == vk::descriptor_type::storage_buffer;
            const auto takes_sampler =
                type == vk::descriptor_type::sampler || type == vk::descriptor_type::combined_image_sampler;
            const auto takes_view = !takes_buffer && type != vk::descriptor_type::sampler;
            if (descriptor.m_buffer.has_value() != takes_buffer ||
                descriptor.m_view.has_value() != takes_view ||
                descriptor.m_sampler.has_value() != takes_sampler) {
                auto given = std::string_view{"an image view"};
                if (descriptor.m_buffer) {
                    given = "a buffer";
                } else if (!descriptor.m_view) {
                    given = "a sampler";
                } else if (descriptor.m_sampler) {
                    given = "an image view and a sampler";
                }

                return std::unexpected(std::format("binding {} is a {} and was given {}",
                                                   descriptor.m_binding, detail::type_name(type), given));
            }

            auto write = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = descriptor.m_binding,
                .dstArrayElement = descriptor.m_element,
                .descriptorCount = 1,
                .descriptorType = static_cast<VkDescriptorType>(type),
            };

            if (takes_buffer) {
                const auto& buffer = descriptor.m_buffer->get().m_state;
                if (!buffer || buffer->device != device) {
                    return std::unexpected(
                        std::format("binding {}: the buffer was moved from or belongs to another device",
                                    descriptor.m_binding));
                }

                const auto needed = type == vk::descriptor_type::uniform_buffer
                                        ? vk::buffer_usage::uniform_buffer
                                        : vk::buffer_usage::storage_buffer;
                if (!vk::has(buffer->usage, needed)) {
                    return std::unexpected(std::format("binding {} is a {}, and {} lacks that usage",
                                                       descriptor.m_binding, detail::type_name(type),
                                                       detail::quoted(buffer->name, "buffer")));
                }

                const auto size =
                    descriptor.m_size.value_or(buffer->size - std::min(descriptor.m_offset, buffer->size));
                if (size == 0 || !detail::fits(buffer->size, descriptor.m_offset, size)) {
                    return std::unexpected(
                        std::format("binding {}: {} bytes at offset {} aren't inside {}, which has {}",
                                    descriptor.m_binding, size, descriptor.m_offset,
                                    detail::quoted(buffer->name, "buffer"), buffer->size));
                }

                write.pBufferInfo = &resolved.buffers.emplace_back(VkDescriptorBufferInfo{
                    .buffer = buffer->handle,
                    .offset = descriptor.m_offset,
                    .range = size,
                });
            } else {
                auto info = VkDescriptorImageInfo{};

                if (takes_sampler) {
                    const auto& sampler = descriptor.m_sampler->get().m_state;
                    if (!sampler || sampler->device != device) {
                        return std::unexpected(
                            std::format("binding {}: the sampler was moved from or belongs to another device",
                                        descriptor.m_binding));
                    }

                    info.sampler = sampler->handle;
                }

                if (takes_view) {
                    const auto& view = descriptor.m_view->get().m_state;
                    if (!view || view->device != device) {
                        return std::unexpected(std::format(
                            "binding {}: the image view was moved from or belongs to another device",
                            descriptor.m_binding));
                    }

                    const auto image = view->image.lock();
                    if (!image) {
                        return std::unexpected(std::format("binding {}: {} was destroyed",
                                                           descriptor.m_binding,
                                                           detail::quoted(view->image_name, "image")));
                    }

                    const auto storing = type == vk::descriptor_type::storage_image;
                    if (!vk::has(image->usage,
                                 storing ? vk::image_usage::storage : vk::image_usage::sampled)) {
                        return std::unexpected(std::format("binding {} is a {}, and {} lacks the {} usage",
                                                           descriptor.m_binding, detail::type_name(type),
                                                           detail::quoted(image->name, "image"),
                                                           storing ? "storage" : "sampled"));
                    }

                    const auto home = image->home;
                    const auto readable = home == vk::access::shader_read || home == vk::access::depth_read;
                    if (home != vk::access::shader_write && home != vk::access::manual &&
                        (storing || !readable)) {
                        return std::unexpected(std::format("binding {} is a {}, and the home of {} is {}",
                                                           descriptor.m_binding, detail::type_name(type),
                                                           detail::quoted(image->name, "image"),
                                                           detail::use_of(home).name));
                    }

                    info.imageView = view->handle;
                    info.imageLayout = storing || home == vk::access::shader_write
                                           ? VK_IMAGE_LAYOUT_GENERAL
                                           : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
                }

                write.pImageInfo = &resolved.images.emplace_back(info);
            }

            resolved.writes.push_back(write);
        }

        return resolved;
    }

    set_layout::set_layout(std::shared_ptr<const set_layout::state> state) noexcept
        : m_state(std::move(state)) {}

    auto set_layout::make(const vk::device& device, const vk::set_layout_options& options)
        -> std::expected<vk::set_layout, rei::error> {
        const auto what = detail::quoted(options.name, "set layout");

        auto state = std::make_shared<set_layout::state>();
        state->device = device.m_state;
        state->bindings = options.bindings;
        state->pushed = options.pushed;
        state->name = options.name;
        std::ranges::sort(state->bindings, {}, &vk::set_binding::binding);

        const auto repeated = std::ranges::adjacent_find(state->bindings, {}, &vk::set_binding::binding);
        if (repeated != state->bindings.end()) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: binding {} is listed twice", what, repeated->binding),
            });
        }
        for (const auto& binding : state->bindings) {
            if (binding.count == 0 || std::to_underlying(binding.stages) == 0) {
                return std::unexpected(rei::error{
                    vk::error_code::invalid_argument,
                    std::format("{}: binding {} has a count of 0 or no stages", what, binding.binding),
                });
            }
        }

        const auto bindings =
            state->bindings |
            std::views::transform([](const vk::set_binding& binding) -> VkDescriptorSetLayoutBinding {
                return {
                    .binding = binding.binding,
                    .descriptorType = static_cast<VkDescriptorType>(binding.type),
                    .descriptorCount = binding.count,
                    .stageFlags = static_cast<VkShaderStageFlags>(binding.stages),
                };
            }) |
            std::ranges::to<std::vector>();
        const auto info = VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags =
                options.pushed
                    ? VkDescriptorSetLayoutCreateFlags{VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT}
                    : VkDescriptorSetLayoutCreateFlags{},
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };

        const auto& context = *state->device;
        if (const auto result =
                context.table.vkCreateDescriptorSetLayout(context.handle, &info, nullptr, &state->handle);
            result != VK_SUCCESS) {
            return std::unexpected(detail::vulkan_error(result, "vkCreateDescriptorSetLayout", what));
        }

        context.name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<std::uint64_t>(state->handle),
                     options.name);

        return vk::set_layout{std::move(state)};
    }

    auto set_layout::handle() const noexcept -> VkDescriptorSetLayout {
        return m_state->handle;
    }

    descriptor_set::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        device->retire({
            .label = detail::label(name, "descriptor set"),
            .descriptor_pool = pool,
            .descriptor_set = handle,
        });
    }

    descriptor_set::descriptor_set(std::shared_ptr<descriptor_set::state> state) noexcept
        : m_state(std::move(state)) {}

    auto descriptor_set::make(const vk::device& device, const vk::descriptor_set_options& options)
        -> std::expected<vk::descriptor_set, rei::error> {
        const auto what = detail::quoted(options.name, "descriptor set");
        const auto& layout = *options.layout.m_state;

        if (layout.device != device.m_state) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: {} belongs to another device", what,
                            detail::quoted(layout.name, "set layout")),
            });
        }
        if (layout.pushed) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: {} is pushed, so it has no sets", what,
                            detail::quoted(layout.name, "set layout")),
            });
        }

        auto state = std::make_shared<descriptor_set::state>();
        state->device = device.m_state;
        state->layout = options.layout.m_state;
        state->name = options.name;

        auto& context = *state->device;
        const auto lock = std::scoped_lock{context.descriptor_mutex};

        auto allocation = VkDescriptorSetAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout.handle,
        };
        for (const VkDescriptorPool pool : context.descriptor_pools | std::views::reverse) {
            allocation.descriptorPool = pool;

            const auto result =
                context.table.vkAllocateDescriptorSets(context.handle, &allocation, &state->handle);
            if (result == VK_SUCCESS) {
                state->pool = pool;
                break;
            }
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
                return std::unexpected(detail::vulkan_error(result, "vkAllocateDescriptorSets", what));
            }
        }

        if (state->pool == VK_NULL_HANDLE) {
            constexpr auto types = std::array{
                vk::descriptor_type::sampler,        vk::descriptor_type::combined_image_sampler,
                vk::descriptor_type::sampled_image,  vk::descriptor_type::storage_image,
                vk::descriptor_type::uniform_buffer, vk::descriptor_type::storage_buffer,
            };
            const auto capacity = std::uint32_t{64} << std::min(context.descriptor_pools.size(), 6uz);
            const auto sizes = types |
                               std::views::transform([&](vk::descriptor_type type) -> VkDescriptorPoolSize {
                                   auto needed = std::uint32_t{};
                                   for (const auto& binding : layout.bindings) {
                                       needed += binding.type == type ? binding.count : 0U;
                                   }

                                   return {
                                       .type = static_cast<VkDescriptorType>(type),
                                       .descriptorCount = std::max(capacity * 4, needed),
                                   };
                               }) |
                               std::ranges::to<std::vector>();
            const auto info = VkDescriptorPoolCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                .maxSets = capacity,
                .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                .pPoolSizes = sizes.data(),
            };

            VkDescriptorPool pool{};
            if (const auto result =
                    context.table.vkCreateDescriptorPool(context.handle, &info, nullptr, &pool);
                result != VK_SUCCESS) {
                return std::unexpected(detail::vulkan_error(result, "vkCreateDescriptorPool", what));
            }
            context.descriptor_pools.push_back(pool);
            context.trace("descriptor pool {}: room for {} sets", context.descriptor_pools.size(), capacity);

            allocation.descriptorPool = pool;
            if (const auto result =
                    context.table.vkAllocateDescriptorSets(context.handle, &allocation, &state->handle);
                result != VK_SUCCESS) {
                return std::unexpected(detail::vulkan_error(result, "vkAllocateDescriptorSets", what));
            }
            state->pool = pool;
        }

        context.name(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<std::uint64_t>(state->handle),
                     options.name);

        return vk::descriptor_set{std::move(state)};
    }

    auto descriptor_set::write(std::initializer_list<vk::descriptor> descriptors)
        -> std::expected<void, rei::error> {
        return write(std::span{descriptors.begin(), descriptors.size()});
    }

    auto descriptor_set::write(std::span<const vk::descriptor> descriptors)
        -> std::expected<void, rei::error> {
        auto& state = *m_state;
        const auto& context = *state.device;

        const auto resolved = state.layout->resolve(descriptors, state.handle);
        if (!resolved) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("write into {}: {}", detail::quoted(state.name, "descriptor set"),
                            resolved.error()),
            });
        }
        context.table.vkUpdateDescriptorSets(context.handle,
                                             static_cast<std::uint32_t>(resolved->writes.size()),
                                             resolved->writes.data(), 0, nullptr);

        for (const auto& descriptor : descriptors) {
            const auto binding = descriptor.m_binding;
            const auto element = descriptor.m_element;
            std::erase_if(state.references, [&](const detail::reference& reference) -> bool {
                return reference.binding == binding && reference.element == element;
            });
            state.samplers.erase({binding, element});

            if (descriptor.m_buffer) {
                const auto& buffer = descriptor.m_buffer->get().m_state;
                state.references.push_back({
                    .binding = binding,
                    .element = element,
                    .target = detail::quoted(buffer->name, "buffer"),
                    .resource = buffer,
                });
            }
            if (descriptor.m_view) {
                const auto& view = descriptor.m_view->get().m_state;
                if (!view->own) {
                    state.references.push_back({
                        .binding = binding,
                        .element = element,
                        .target = detail::quoted(view->name, "image view"),
                        .resource = view,
                    });
                }
                state.references.push_back({
                    .binding = binding,
                    .element = element,
                    .target = detail::quoted(view->image_name, "image"),
                    .resource = view->image,
                });
            }
            if (descriptor.m_sampler) {
                state.samplers.insert_or_assign({binding, element}, descriptor.m_sampler->get());
            }
        }

        return {};
    }

    auto descriptor_set::handle() const noexcept -> VkDescriptorSet {
        return m_state->handle;
    }
} // namespace rei::gpu::vk
