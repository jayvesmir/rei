#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/detail/features.hpp"
#include "rei/gpu/vk/detail/formats.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/pipeline.hpp"

namespace rei::gpu::vk {
    namespace {
        auto describe(const detail::capability_enable& enable) -> std::string {
            return std::visit(
                []<typename EnableType>(const EnableType& alternative) -> std::string {
                    if constexpr (std::is_same_v<EnableType, vk::feature>) {
                        return std::format("the feature {}",
                                           detail::feature_table[std::to_underlying(alternative)].name);
                    } else if constexpr (std::is_same_v<EnableType, detail::extension_enable>) {
                        return std::format("the extension {}", alternative.name);
                    } else {
                        return std::string{alternative.name};
                    }
                },
                enable);
        }
    } // namespace

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

            for (const auto capability : source.capabilities) {
                const auto needed =
                    std::ranges::find(detail::capability_table, capability, &detail::capability_info::value);
                if (needed == detail::capability_table.end() ||
                    std::ranges::any_of(needed->enables,
                                        [&](const detail::capability_enable& enable) -> bool {
                                            return context.allows(enable);
                                        })) {
                    continue;
                }

                const auto alternatives =
                    needed->enables | std::views::transform(describe) | std::ranges::to<std::vector>();
                auto needs = std::string{"which rei can't enable"};
                if (!alternatives.empty()) {
                    const auto others = std::span{alternatives}.first(alternatives.size() - 1) |
                                        std::views::join_with(std::string_view{", "}) |
                                        std::ranges::to<std::string>();
                    needs = std::format("which needs {}{}{}", others, others.empty() ? "" : " or ",
                                        alternatives.back());
                }

                return std::unexpected(rei::error{
                    vk::error_code::missing_feature,
                    std::format("{}: the {} shader declares {}, {}", what, stage, needed->name, needs),
                });
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

    graphics_pipeline::state::~state() {
        if (handle == VK_NULL_HANDLE && layout == VK_NULL_HANDLE) {
            return;
        }

        if (writes && handle != VK_NULL_HANDLE) {
            device->storage_writers--;
        }

        device->retire({
            .label = handle != VK_NULL_HANDLE ? detail::label(name, kind) : std::string{},
            .pipeline = handle,
            .pipeline_layout = layout,
        });
    }

    graphics_pipeline::graphics_pipeline(std::shared_ptr<const graphics_pipeline::state> state) noexcept
        : m_state(std::move(state)) {}

    auto graphics_pipeline::make(const vk::device& device, const vk::graphics_pipeline_options& options)
        -> std::expected<vk::graphics_pipeline, rei::error> {
        const auto& context = *device.m_state;
        const auto& vertex = *options.vertex.m_state;
        const auto what = detail::quoted(options.name, "graphics pipeline");

        if (vertex.stage != vk::shader_stage::vertex) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the entry point '{}' isn't a vertex shader", what, vertex.entry_point),
            });
        }
        if (options.fragment && options.fragment->m_state->stage != vk::shader_stage::fragment) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the entry point '{}' isn't a fragment shader", what,
                            options.fragment->m_state->entry_point),
            });
        }
        if (!std::has_single_bit(options.samples) || options.samples > 64) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: {} samples, and that isn't a power of two up to 64", what, options.samples),
            });
        }
        if (options.polygon == vk::polygon_mode::line && !device.has(vk::feature::fill_mode_non_solid)) {
            return std::unexpected(rei::error{
                vk::error_code::missing_feature,
                std::format("{}: polygon_mode::line needs the feature fill_mode_non_solid", what),
            });
        }
        if (options.depth &&
            (detail::info_of(options.depth->format).aspects & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the depth format {} has no depth", what,
                            detail::info_of(options.depth->format).name),
            });
        }

        for (const auto& output : options.color) {
            if (detail::info_of(output.format).aspects != VK_IMAGE_ASPECT_COLOR_BIT) {
                return std::unexpected(rei::error{
                    vk::error_code::invalid_argument,
                    std::format("{}: the color format {} has no color", what,
                                detail::info_of(output.format).name),
                });
            }

            const auto blend = output.blend.value_or(vk::blend{});
            const auto factors =
                std::array{blend.src_color, blend.dst_color, blend.src_alpha, blend.dst_alpha};
            if (std::ranges::any_of(
                    factors,
                    [](vk::blend_factor factor) -> bool { return factor >= vk::blend_factor::src1_color; }) &&
                !device.has(vk::feature::dual_src_blend)) {
                return std::unexpected(rei::error{
                    vk::error_code::missing_feature,
                    std::format("{}: blending with a second source needs the feature dual_src_blend", what),
                });
            }
        }

        auto bindings = std::vector<VkVertexInputBindingDescription>{};
        auto attributes = std::vector<VkVertexInputAttributeDescription>{};
        for (const auto& binding : options.vertex_input) {
            for (const auto& attribute : binding.attributes) {
                if (std::ranges::contains(attributes, attribute.location,
                                          &VkVertexInputAttributeDescription::location)) {
                    return std::unexpected(rei::error{
                        vk::error_code::invalid_argument,
                        std::format("{}: vertex_input has location {} twice", what, attribute.location),
                    });
                }

                attributes.push_back({
                    .location = attribute.location,
                    .binding = static_cast<std::uint32_t>(bindings.size()),
                    .format = static_cast<VkFormat>(attribute.format),
                    .offset = attribute.offset,
                });
            }

            bindings.push_back({
                .binding = static_cast<std::uint32_t>(bindings.size()),
                .stride = binding.stride,
                .inputRate =
                    binding.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            });
        }

        for (const auto& input : vertex.inputs) {
            const auto given =
                std::ranges::find(attributes, input.location, &VkVertexInputAttributeDescription::location);
            if (given == attributes.end()) {
                return std::unexpected(rei::error{
                    vk::error_code::layout_mismatch,
                    std::format("{}: the vertex shader reads location {} ('{}'), and vertex_input has no "
                                "attribute there",
                                what, input.location, input.name),
                });
            }

            const auto expected = detail::info_of(input.format);
            const auto provided = detail::info_of(static_cast<vk::format>(given->format));
            if (input.format != vk::format::undefined && expected.numeric != provided.numeric) {
                return std::unexpected(rei::error{
                    vk::error_code::layout_mismatch,
                    std::format("{}: location {} ('{}') is a {} in the vertex shader and a {} in "
                                "vertex_input",
                                what, input.location, input.name, expected.name, provided.name),
                });
            }
        }

        auto state = std::make_shared<graphics_pipeline::state>();
        state->kind = "graphics pipeline";
        state->name = options.name;
        state->device = device.m_state;
        state->targets = {
            .color = options.color | std::views::transform(&vk::color_output::format) |
                     std::ranges::to<std::vector>(),
            .depth = options.depth.transform(
                [](const vk::depth_output& output) -> vk::format { return output.format; }),
            .samples = options.samples,
        };
        state->vertex_inputs = static_cast<std::uint32_t>(bindings.size());

        auto shaders = std::vector{options.vertex};
        if (options.fragment) {
            shaders.push_back(*options.fragment);
        }
        if (const auto laid_out = set_layout::state::lay_out(*state, context, shaders, options.sets,
                                                             options.push_constant_size);
            !laid_out) {
            return std::unexpected(laid_out.error());
        }

        const auto modules = shaders |
                             std::views::transform([](const vk::shader& shader) -> VkShaderModuleCreateInfo {
                                 return {
                                     .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = shader.m_state->spirv.size() * sizeof(std::uint32_t),
                                     .pCode = shader.m_state->spirv.data(),
                                 };
                             }) |
                             std::ranges::to<std::vector>();
        auto stages = std::vector<VkPipelineShaderStageCreateInfo>{};
        for (const auto [shader, module] : std::views::zip(shaders, modules)) {
            stages.push_back({
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = &module,
                .stage = static_cast<VkShaderStageFlagBits>(shader.m_state->stage),
                .pName = shader.m_state->entry_point.c_str(),
            });
        }

        const auto vertex_input = VkPipelineVertexInputStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size()),
            .pVertexBindingDescriptions = bindings.data(),
            .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
            .pVertexAttributeDescriptions = attributes.data(),
        };
        const auto assembly = VkPipelineInputAssemblyStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = static_cast<VkPrimitiveTopology>(options.topology),
        };
        const auto viewport = VkPipelineViewportStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        const auto bias = options.depth_bias.value_or(vk::depth_bias{});
        const auto rasterization = VkPipelineRasterizationStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = static_cast<VkPolygonMode>(options.polygon),
            .cullMode = static_cast<VkCullModeFlags>(options.cull),
            .frontFace = static_cast<VkFrontFace>(options.front),
            .depthBiasEnable = options.depth_bias ? VK_TRUE : VK_FALSE,
            .depthBiasConstantFactor = bias.constant,
            .depthBiasClamp = bias.clamp,
            .depthBiasSlopeFactor = bias.slope,
            .lineWidth = 1.0f,
        };
        const auto multisample = VkPipelineMultisampleStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = static_cast<VkSampleCountFlagBits>(options.samples),
        };
        const auto depth = options.depth.value_or(
            vk::depth_output{.format = vk::format::undefined, .test = false, .write = false});
        const auto depth_stencil = VkPipelineDepthStencilStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = depth.test ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = depth.write ? VK_TRUE : VK_FALSE,
            .depthCompareOp = static_cast<VkCompareOp>(depth.compare),
        };
        const auto blends =
            options.color |
            std::views::transform([](const vk::color_output& output) -> VkPipelineColorBlendAttachmentState {
                const auto blend = output.blend.value_or(vk::blend{});
                return {
                    .blendEnable = output.blend ? VK_TRUE : VK_FALSE,
                    .srcColorBlendFactor = static_cast<VkBlendFactor>(blend.src_color),
                    .dstColorBlendFactor = static_cast<VkBlendFactor>(blend.dst_color),
                    .colorBlendOp = static_cast<VkBlendOp>(blend.color_op),
                    .srcAlphaBlendFactor = static_cast<VkBlendFactor>(blend.src_alpha),
                    .dstAlphaBlendFactor = static_cast<VkBlendFactor>(blend.dst_alpha),
                    .alphaBlendOp = static_cast<VkBlendOp>(blend.alpha_op),
                    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                };
            }) |
            std::ranges::to<std::vector>();
        const auto color_blend = VkPipelineColorBlendStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = static_cast<std::uint32_t>(blends.size()),
            .pAttachments = blends.data(),
        };
        constexpr auto dynamic_states = std::array{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const auto dynamic = VkPipelineDynamicStateCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const auto color_formats = state->targets.color |
                                   std::views::transform([](vk::format format) -> VkFormat {
                                       return static_cast<VkFormat>(format);
                                   }) |
                                   std::ranges::to<std::vector>();
        const auto rendering = VkPipelineRenderingCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = static_cast<std::uint32_t>(color_formats.size()),
            .pColorAttachmentFormats = color_formats.data(),
            .depthAttachmentFormat = static_cast<VkFormat>(depth.format),
        };
        const auto info = VkGraphicsPipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_stencil,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic,
            .layout = state->layout,
        };
        if (const auto result = context.table.vkCreateGraphicsPipelines(context.handle, VK_NULL_HANDLE, 1,
                                                                        &info, nullptr, &state->handle);
            result != VK_SUCCESS) {
            return std::unexpected(detail::vulkan_error(result, "vkCreateGraphicsPipelines", what));
        }

        if (state->writes) {
            state->device->storage_writers++;
        }
        context.name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(state->handle), options.name);

        return vk::graphics_pipeline{std::move(state)};
    }

    auto graphics_pipeline::adopt(const vk::device& device, VkPipeline pipeline, VkPipelineLayout layout)
        -> vk::graphics_pipeline {
        auto state = std::make_shared<graphics_pipeline::state>();
        state->kind = "graphics pipeline";
        state->device = device.m_state;
        state->handle = pipeline;
        state->layout = layout;
        state->writes = true;
        state->adopted = true;

        if (pipeline != VK_NULL_HANDLE) {
            state->device->storage_writers++;
        }

        return vk::graphics_pipeline{std::move(state)};
    }

    auto graphics_pipeline::handle() const noexcept -> VkPipeline {
        return m_state->handle;
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
