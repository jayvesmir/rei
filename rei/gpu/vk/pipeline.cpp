#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/pipeline.hpp"

namespace rei::gpu::vk {
    auto set_layout::state::lay_out(detail::pipeline_state& pipeline, const vk::device::state& context,
                                    std::span<const vk::shader> shaders, std::span<const vk::set_layout> sets,
                                    std::uint32_t push_constant_size) -> std::expected<void, rei::error> {
        const auto what = detail::quoted(pipeline.name, pipeline.kind);

        if (push_constant_size % 4 != 0 || push_constant_size > context.limits.maxPushConstantsSize) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: push_constant_size is {}, and it has to be a multiple of 4 up to {}", what,
                            push_constant_size, context.limits.maxPushConstantsSize),
            });
        }
        for (const auto& layout : sets) {
            if (layout.m_state->device.get() != &context) {
                return std::unexpected(rei::error{
                    vk::error_code::invalid_argument,
                    std::format("{}: {} belongs to another device", what,
                                detail::quoted(layout.m_state->name, "set layout")),
                });
            }
        }

        pipeline.sets.assign_range(sets);
        pipeline.reads.resize(sets.size());
        pipeline.push_constant_size = push_constant_size;

        for (const auto& shader : shaders) {
            const auto& source = *shader.m_state;
            auto stage = std::string_view{"compute"};
            if (source.stage == vk::shader_stage::vertex) {
                stage = "vertex";
            } else if (source.stage == vk::shader_stage::fragment) {
                stage = "fragment";
            }

            if (source.push_constant_size > push_constant_size) {
                return std::unexpected(rei::error{
                    vk::error_code::layout_mismatch,
                    std::format("{}: the {} shader's push constants end at byte {}, and push_constant_size "
                                "is {}",
                                what, stage, source.push_constant_size, push_constant_size),
                });
            }

            for (const auto& used : source.bindings) {
                const auto where = std::format("set {} binding {} ('{}')", used.set, used.binding, used.name);
                if (used.set >= sets.size()) {
                    return std::unexpected(rei::error{
                        vk::error_code::layout_mismatch,
                        std::format("{}: the {} shader uses {}, and the pipeline has {} set layouts", what,
                                    stage, where, sets.size()),
                    });
                }

                const auto& layout = *sets[used.set].m_state;
                const auto described = detail::quoted(layout.name, "set layout");
                const auto declared =
                    std::ranges::find(layout.bindings, used.binding, &vk::set_binding::binding);
                if (declared == layout.bindings.end()) {
                    return std::unexpected(rei::error{
                        vk::error_code::layout_mismatch,
                        std::format("{}: the {} shader uses {} as a {}, and {} has no such binding", what,
                                    stage, where, detail::type_name(used.type), described),
                    });
                }
                if (declared->type != used.type) {
                    return std::unexpected(rei::error{
                        vk::error_code::layout_mismatch,
                        std::format("{}: {} is a {} in the {} shader and a {} in {}", what, where,
                                    detail::type_name(used.type), stage, detail::type_name(declared->type),
                                    described),
                    });
                }
                if (used.count > declared->count) {
                    return std::unexpected(rei::error{
                        vk::error_code::layout_mismatch,
                        std::format("{}: {} has {} elements in the {} shader and {} in {}", what, where,
                                    used.count, stage, declared->count, described),
                    });
                }
                if (!vk::has(declared->stages, source.stage)) {
                    return std::unexpected(rei::error{
                        vk::error_code::layout_mismatch,
                        std::format("{}: {} isn't visible to the {} stage in {}", what, where, stage,
                                    described),
                    });
                }

                if (!std::ranges::contains(pipeline.reads[used.set], used.binding)) {
                    pipeline.reads[used.set].push_back(used.binding);
                }
            }

            pipeline.writes = pipeline.writes || source.writes;
        }

        const auto layouts = sets |
                             std::views::transform([](const vk::set_layout& layout) -> VkDescriptorSetLayout {
                                 return layout.handle();
                             }) |
                             std::ranges::to<std::vector>();
        const auto constants = VkPushConstantRange{
            .stageFlags = VK_SHADER_STAGE_ALL,
            .offset = 0,
            .size = push_constant_size,
        };
        const auto info = VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<std::uint32_t>(layouts.size()),
            .pSetLayouts = layouts.data(),
            .pushConstantRangeCount = push_constant_size > 0 ? 1U : 0U,
            .pPushConstantRanges = &constants,
        };
        if (const auto result =
                context.table.vkCreatePipelineLayout(context.handle, &info, nullptr, &pipeline.layout);
            result != VK_SUCCESS) {
            return std::unexpected(detail::vulkan_error(result, "vkCreatePipelineLayout", what));
        }

        return {};
    }

    compute_pipeline::state::~state() {
        if (handle == VK_NULL_HANDLE && layout == VK_NULL_HANDLE) {
            return;
        }

        device->retire({
            .label = handle != VK_NULL_HANDLE ? detail::label(name, kind) : std::string{},
            .pipeline = handle,
            .pipeline_layout = layout,
        });
    }

    compute_pipeline::compute_pipeline(std::shared_ptr<const compute_pipeline::state> state) noexcept
        : m_state(std::move(state)) {}

    auto compute_pipeline::make(const vk::device& device, const vk::compute_pipeline_options& options)
        -> std::expected<vk::compute_pipeline, rei::error> {
        const auto& context = *device.m_state;
        const auto& shader = *options.shader.m_state;
        const auto what = detail::quoted(options.name, "compute pipeline");

        if (shader.stage != vk::shader_stage::compute) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the entry point '{}' isn't a compute shader", what, shader.entry_point),
            });
        }

        auto state = std::make_shared<compute_pipeline::state>();
        state->kind = "compute pipeline";
        state->name = options.name;
        state->device = device.m_state;

        if (const auto laid_out = set_layout::state::lay_out(*state, context, std::array{options.shader},
                                                             options.sets, options.push_constant_size);
            !laid_out) {
            return std::unexpected(laid_out.error());
        }

        const auto module = VkShaderModuleCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shader.spirv.size() * sizeof(std::uint32_t),
            .pCode = shader.spirv.data(),
        };
        const auto info = VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = &module,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .pName = shader.entry_point.c_str(),
                },
            .layout = state->layout,
        };
        if (const auto result = context.table.vkCreateComputePipelines(context.handle, VK_NULL_HANDLE, 1,
                                                                       &info, nullptr, &state->handle);
            result != VK_SUCCESS) {
            return std::unexpected(detail::vulkan_error(result, "vkCreateComputePipelines", what));
        }

        context.name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(state->handle), options.name);

        return vk::compute_pipeline{std::move(state)};
    }

    auto compute_pipeline::adopt(const vk::device& device, VkPipeline pipeline, VkPipelineLayout layout)
        -> vk::compute_pipeline {
        auto state = std::make_shared<compute_pipeline::state>();
        state->kind = "compute pipeline";
        state->device = device.m_state;
        state->handle = pipeline;
        state->layout = layout;
        state->writes = true;
        state->adopted = true;

        return vk::compute_pipeline{std::move(state)};
    }

    auto compute_pipeline::handle() const noexcept -> VkPipeline {
        return m_state->handle;
    }
} // namespace rei::gpu::vk
