#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/formats.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    auto commands::render(const vk::render_options& options) -> vk::render_pass {
        auto& self = *m_state;

        if (!self.outside("render")) {
            return vk::render_pass{*this};
        }

        auto scope = state::render_scope{};
        scope.label = detail::quoted(options.name, "render pass");
        scope.workflow = options.name.empty() ? "render" : std::format("render {}", options.name);
        scope.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;

        constexpr auto unbounded = std::numeric_limits<std::uint32_t>::max();
        auto extent = vk::extent{.width = unbounded, .height = unbounded};
        auto layers = std::optional<std::uint32_t>{};
        auto samples = std::optional<std::uint32_t>{};

        const auto admit = [&](const vk::image_view& target, vk::image_usage usage,
                               bool resolving) -> std::shared_ptr<image::state> {
            const auto verb = std::string_view{resolving ? "resolve into" : "render into"};
            if (!self.usable(target.m_state, std::format("{} an image view in {}", verb, scope.label),
                             "image view")) {
                return nullptr;
            }

            const auto& view = *target.m_state;
            const auto call = std::format("{} {} in {}", verb, view.quoted(), scope.label);
            auto image = self.alive(call, view);
            if (!image) {
                return nullptr;
            }

            const auto coloring = usage == vk::image_usage::color_attachment;
            const auto format = detail::info_of(image->format);
            const auto wanted = resolving ? std::optional{1U} : samples;

            if (!vk::has(image->usage, usage)) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{}: its usage lacks {}", call,
                                      coloring ? "color_attachment" : "depth_stencil_attachment"));
                return nullptr;
            }
            if ((format.aspects & (coloring ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT)) == 0) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{}: {} has no {}", call, format.name, coloring ? "color" : "depth"));
                return nullptr;
            }
            if (view.range.levelCount != 1 || image->extent.depth != 1) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{}: a target is a view of one mip level of a 2D image, and this one "
                                      "has {} levels and a depth of {}",
                                      call, view.range.levelCount, image->extent.depth));
                return nullptr;
            }
            if (layers && view.range.layerCount != *layers) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{}: it has {} array layers, and the targets before it have {}", call,
                                      view.range.layerCount, *layers));
                return nullptr;
            }
            if (wanted && image->samples != *wanted) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{}: it has {} samples, and {} {}", call, image->samples,
                                      resolving ? "a resolve target has" : "the targets before it have",
                                      *wanted));
                return nullptr;
            }

            extent.width = std::min(extent.width, view.extent.width);
            extent.height = std::min(extent.height, view.extent.height);
            layers = view.range.layerCount;
            if (!resolving) {
                samples = image->samples;
            }

            return image;
        };

        auto colors = std::vector<VkRenderingAttachmentInfo>{};
        for (const auto& target : options.color) {
            const auto image = admit(target.target, vk::image_usage::color_attachment, false);
            if (!image) {
                return vk::render_pass{*this};
            }

            const auto& clear = target.clear;
            scope.formats.color.push_back(image->format);
            scope.targets.push_back({
                .image = image,
                .range = target.target.m_state->range,
                .needed = vk::access::color_attachment,
                .overwritten = target.load != vk::load_op::load,
                .wrote = true,
            });
            colors.push_back({
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = target.target.handle(),
                .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .loadOp = static_cast<VkAttachmentLoadOp>(target.load),
                .storeOp = static_cast<VkAttachmentStoreOp>(target.store),
                .clearValue = {.color = {.float32 = {clear[0], clear[1], clear[2], clear[3]}}},
            });

            if (!target.resolve) {
                continue;
            }

            const auto& resolve = target.resolve->target;
            const auto resolved = admit(resolve, vk::image_usage::color_attachment, true);
            if (!resolved) {
                return vk::render_pass{*this};
            }
            if (image->samples == 1 || resolved->format != image->format) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("resolve into {} in {}: it takes a multisampled target of its own "
                                      "format, and {} is {} with {} samples",
                                      resolve.m_state->quoted(), scope.label, target.target.m_state->quoted(),
                                      detail::info_of(image->format).name, image->samples));
                return vk::render_pass{*this};
            }

            scope.targets.push_back({
                .image = resolved,
                .range = resolve.m_state->range,
                .needed = vk::access::color_attachment,
                .overwritten = true,
                .wrote = true,
            });
            colors.back().resolveMode = detail::info_of(image->format).numeric == detail::numeric::floating
                                            ? VK_RESOLVE_MODE_AVERAGE_BIT
                                            : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            colors.back().resolveImageView = resolve.handle();
            colors.back().resolveImageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        }

        auto depth = VkRenderingAttachmentInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (options.depth) {
            const auto& target = *options.depth;
            const auto image = admit(target.target, vk::image_usage::depth_stencil_attachment, false);
            if (!image) {
                return vk::render_pass{*this};
            }

            const auto needed = target.read_only ? vk::access::depth_read : vk::access::depth_attachment;
            scope.formats.depth = image->format;
            scope.targets.push_back({
                .image = image,
                .range = target.target.m_state->range,
                .needed = needed,
                .overwritten = !target.read_only && target.load != vk::load_op::load,
                .wrote = !target.read_only,
            });

            depth.imageView = target.target.handle();
            depth.imageLayout = detail::use_of(needed).layout;
            depth.loadOp =
                target.read_only ? VK_ATTACHMENT_LOAD_OP_LOAD : static_cast<VkAttachmentLoadOp>(target.load);
            depth.storeOp = target.read_only ? VK_ATTACHMENT_STORE_OP_NONE
                                             : static_cast<VkAttachmentStoreOp>(target.store);
            depth.clearValue = {.depthStencil = {.depth = target.clear}};
        }

        const auto& limits = self.device->limits;
        if (scope.targets.empty() || colors.size() > limits.maxColorAttachments) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: it has {} color targets and {} depth target, and it needs a target "
                                  "and at most {} color targets",
                                  scope.label, colors.size(), options.depth ? "a" : "no",
                                  limits.maxColorAttachments));
            return vk::render_pass{*this};
        }

        const auto area = options.area.value_or(vk::area{.width = extent.width, .height = extent.height});
        if (area.x < 0 || area.y < 0 || area.width == 0 || area.height == 0 ||
            !detail::fits(extent.width, static_cast<std::uint32_t>(area.x), area.width) ||
            !detail::fits(extent.height, static_cast<std::uint32_t>(area.y), area.height)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{}: the area {}x{} at {},{} isn't inside its targets of {}x{}",
                                  scope.label, area.width, area.height, area.x, area.y, extent.width,
                                  extent.height));
            return vk::render_pass{*this};
        }

        scope.formats.samples = *samples;

        self.begin(scope.workflow);
        for (const auto& target : scope.targets) {
            const auto size = target.image->extent_of(target.range.baseMipLevel);
            const auto whole =
                area.x == 0 && area.y == 0 && area.width == size.width && area.height == size.height;
            self.enter(scope.workflow, *target.image, target.range, target.needed,
                       target.overwritten && whole);
        }
        if (self.device->storage_writers > 0) {
            self.add(scope.workflow,
                     {.target = "memory", .from = detail::earlier_work, .to = detail::render_work});
            scope.waited = true;
        }
        self.flush(scope.workflow);

        const auto rectangle = VkRect2D{
            .offset = {.x = area.x, .y = area.y},
            .extent = {.width = area.width, .height = area.height},
        };
        const auto info = VkRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = rectangle,
            .layerCount = *layers,
            .colorAttachmentCount = static_cast<std::uint32_t>(colors.size()),
            .pColorAttachments = colors.data(),
            .pDepthAttachment = options.depth ? &depth : nullptr,
        };
        const auto viewport = VkViewport{
            .width = static_cast<float>(extent.width),
            .height = static_cast<float>(extent.height),
            .maxDepth = 1.0f,
        };
        self.device->table.vkCmdBeginRendering(self.pool.buffer, &info);
        self.device->table.vkCmdSetViewport(self.pool.buffer, 0, 1, &viewport);
        self.device->table.vkCmdSetScissor(self.pool.buffer, 0, 1, &rectangle);

        self.render.emplace(std::move(scope));

        return vk::render_pass{*this};
    }

    render_pass::render_pass(vk::commands& commands) noexcept : m_commands(commands) {}

    render_pass::~render_pass() {
        end();
    }

    auto render_pass::_drawing(std::string_view call) -> bool {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, call, "render pass") || !self.readable(*self.render, call)) {
            return false;
        }

        auto& scope = *self.render;
        const auto& pipeline = *scope.pipeline;

        for (auto binding = 0U; binding < scope.vertex_inputs; binding++) {
            if (binding >= scope.vertex_buffers.size() || !scope.vertex_buffers[binding]) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{} in {}: {} reads vertex buffer {}, and none is bound there", call,
                                      scope.label, detail::quoted(pipeline.name, pipeline.kind), binding));
                return false;
            }
        }
        if (pipeline.writes && !scope.waited) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{} in {}: {} writes storage and was made after the pass began", call,
                                  scope.label, detail::quoted(pipeline.name, pipeline.kind)));
            return false;
        }

        scope.wrote = scope.wrote || pipeline.writes;

        return true;
    }

    auto render_pass::_drawing_indirect(std::string_view call, const vk::buffer& arguments,
                                        std::uint64_t offset, std::uint32_t count, std::uint64_t stride)
        -> bool {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, call, "render pass") ||
            !self.usable(arguments.m_state, std::format("{} in a render pass", call), "buffer")) {
            return false;
        }

        const auto& buffer = *arguments.m_state;
        const auto& context = *self.device;
        if (!vk::has(buffer.usage, vk::buffer_usage::indirect_buffer) || offset % 4 != 0 ||
            !detail::fits(buffer.size, offset, count * stride)) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{} in {}: {} needs the indirect_buffer usage and {} bytes at an offset "
                                  "that is a multiple of 4, and it has {} at offset {}",
                                  call, self.render->label, detail::quoted(buffer.name, "buffer"),
                                  count * stride, buffer.size, offset));
            return false;
        }
        if (count > context.limits.maxDrawIndirectCount ||
            (count > 1 && !context.features[std::to_underlying(vk::feature::multi_draw_indirect)])) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{} in {}: {} draws, and '{}' allows {} with the feature "
                                  "multi_draw_indirect and 1 without",
                                  call, self.render->label, count, context.physical.name,
                                  context.limits.maxDrawIndirectCount));
            return false;
        }

        return _drawing(call);
    }

    auto render_pass::_push_constants(std::span<const std::byte> data) -> void {
        auto& self = *m_commands.m_state;

        if (self.inside(m_open, "push_constants", "render pass")) {
            self.push_constants(*self.render, data);
        }
    }

    auto render_pass::bind(const vk::graphics_pipeline& pipeline) -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "bind", "render pass") ||
            !self.usable(pipeline.m_state, "bind in a render pass", "graphics pipeline")) {
            return;
        }

        auto& scope = *self.render;
        const auto& bound = *pipeline.m_state;

        if (!bound.adopted && bound.targets != scope.formats) {
            const auto describe = [](const detail::target_formats& formats) -> std::string {
                return std::format(
                    "color [{}], depth {}, {} samples",
                    formats.color | std::views::transform([](vk::format format) -> std::string_view {
                        return detail::info_of(format).name;
                    }) | std::views::join_with(std::string_view{", "}) |
                        std::ranges::to<std::string>(),
                    formats.depth ? detail::info_of(*formats.depth).name : "none", formats.samples);
            };
            self.fail(vk::error_code::invalid_argument,
                      std::format("bind of {} in {}: it was made for {}, and the pass has {}",
                                  detail::quoted(bound.name, bound.kind), scope.label,
                                  describe(bound.targets), describe(scope.formats)));
            return;
        }

        scope.pipeline = pipeline.m_state;
        scope.sets.resize(std::max(scope.sets.size(), bound.sets.size()));
        scope.vertex_inputs = bound.vertex_inputs;

        self.device->table.vkCmdBindPipeline(self.pool.buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bound.handle);
    }

    auto render_pass::bind(std::uint32_t set, const vk::descriptor_set& descriptors) -> void {
        auto& self = *m_commands.m_state;

        if (self.inside(m_open, "bind", "render pass") &&
            self.usable(descriptors.m_state, "bind in a render pass", "descriptor set")) {
            self.bind(*self.render, set, descriptors.m_state);
        }
    }

    auto render_pass::push_descriptors(std::uint32_t set, std::initializer_list<vk::descriptor> descriptors)
        -> void {
        push_descriptors(set, std::span{descriptors.begin(), descriptors.size()});
    }

    auto render_pass::push_descriptors(std::uint32_t set, std::span<const vk::descriptor> descriptors)
        -> void {
        auto& self = *m_commands.m_state;

        if (self.inside(m_open, "push_descriptors", "render pass")) {
            self.push_descriptors(*self.render, set, descriptors);
        }
    }

    auto render_pass::bind_vertex_buffer(std::uint32_t binding, const vk::buffer& buffer,
                                         std::uint64_t offset) -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "bind_vertex_buffer", "render pass") ||
            !self.usable(buffer.m_state, "bind_vertex_buffer in a render pass", "buffer")) {
            return;
        }

        auto& scope = *self.render;
        const auto& bound = *buffer.m_state;
        const auto& context = *self.device;

        if (binding >= context.limits.maxVertexInputBindings) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("bind_vertex_buffer in {}: binding {}, and '{}' has {}", scope.label,
                                  binding, context.physical.name, context.limits.maxVertexInputBindings));
            return;
        }
        if (!vk::has(bound.usage, vk::buffer_usage::vertex_buffer) || offset >= bound.size) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("bind_vertex_buffer in {}: {} needs the vertex_buffer usage and bytes "
                                  "past offset {}, and it has {}",
                                  scope.label, detail::quoted(bound.name, "buffer"), offset, bound.size));
            return;
        }

        scope.vertex_buffers.resize(std::max<std::size_t>(scope.vertex_buffers.size(), binding + 1));
        scope.vertex_buffers[binding] = true;

        context.table.vkCmdBindVertexBuffers(self.pool.buffer, binding, 1, &bound.handle, &offset);
    }

    auto render_pass::bind_index_buffer(const vk::buffer& buffer, vk::index_type type, std::uint64_t offset)
        -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "bind_index_buffer", "render pass") ||
            !self.usable(buffer.m_state, "bind_index_buffer in a render pass", "buffer")) {
            return;
        }

        auto& scope = *self.render;
        const auto& bound = *buffer.m_state;
        const auto size = type == vk::index_type::uint16 ? std::uint32_t{2} : std::uint32_t{4};

        if (!vk::has(bound.usage, vk::buffer_usage::index_buffer) || offset % size != 0 ||
            offset >= bound.size) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("bind_index_buffer in {}: {} needs the index_buffer usage and bytes past "
                                  "an offset that is a multiple of {}, and it has {} at offset {}",
                                  scope.label, detail::quoted(bound.name, "buffer"), size, bound.size,
                                  offset));
            return;
        }

        scope.index_buffer = true;

        self.device->table.vkCmdBindIndexBuffer(self.pool.buffer, bound.handle, offset,
                                                static_cast<VkIndexType>(type));
    }

    auto render_pass::viewport(const vk::area& area, float min_depth, float max_depth) -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "viewport", "render pass")) {
            return;
        }

        const auto viewport = VkViewport{
            .x = static_cast<float>(area.x),
            .y = static_cast<float>(area.y),
            .width = static_cast<float>(area.width),
            .height = static_cast<float>(area.height),
            .minDepth = min_depth,
            .maxDepth = max_depth,
        };
        self.device->table.vkCmdSetViewport(self.pool.buffer, 0, 1, &viewport);
    }

    auto render_pass::scissor(const vk::area& area) -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "scissor", "render pass")) {
            return;
        }

        const auto rectangle = VkRect2D{
            .offset = {.x = area.x, .y = area.y},
            .extent = {.width = area.width, .height = area.height},
        };
        self.device->table.vkCmdSetScissor(self.pool.buffer, 0, 1, &rectangle);
    }

    auto render_pass::draw(const vk::draw_options& options) -> void {
        if (!_drawing("draw")) {
            return;
        }

        const auto& self = *m_commands.m_state;
        self.device->table.vkCmdDraw(self.pool.buffer, options.vertices, options.instances,
                                     options.first_vertex, options.first_instance);
    }

    auto render_pass::draw_indexed(const vk::draw_indexed_options& options) -> void {
        if (!_drawing("draw_indexed")) {
            return;
        }

        auto& self = *m_commands.m_state;
        if (!self.render->index_buffer) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("draw_indexed in {}: no index buffer is bound", self.render->label));
            return;
        }

        self.device->table.vkCmdDrawIndexed(self.pool.buffer, options.indices, options.instances,
                                            options.first_index, options.vertex_offset,
                                            options.first_instance);
    }

    auto render_pass::draw_indirect(const vk::buffer& arguments, std::uint64_t offset, std::uint32_t count)
        -> void {
        if (!_drawing_indirect("draw_indirect", arguments, offset, count, sizeof(VkDrawIndirectCommand))) {
            return;
        }

        const auto& self = *m_commands.m_state;
        self.device->table.vkCmdDrawIndirect(self.pool.buffer, arguments.handle(), offset, count,
                                             sizeof(VkDrawIndirectCommand));
    }

    auto render_pass::draw_indexed_indirect(const vk::buffer& arguments, std::uint64_t offset,
                                            std::uint32_t count) -> void {
        if (!_drawing_indirect("draw_indexed_indirect", arguments, offset, count,
                               sizeof(VkDrawIndexedIndirectCommand))) {
            return;
        }

        auto& self = *m_commands.m_state;
        if (!self.render->index_buffer) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("draw_indexed_indirect in {}: no index buffer is bound", self.render->label));
            return;
        }

        self.device->table.vkCmdDrawIndexedIndirect(self.pool.buffer, arguments.handle(), offset, count,
                                                    sizeof(VkDrawIndexedIndirectCommand));
    }

    auto render_pass::end() -> void {
        if (!std::exchange(m_open, false) || !m_commands.m_state || m_commands.m_state->failure) {
            return;
        }

        auto& self = *m_commands.m_state;
        const auto& scope = *self.render;

        self.device->table.vkCmdEndRendering(self.pool.buffer);
        for (const auto& target : scope.targets) {
            self.leave(scope.workflow, *target.image, target.range, target.needed, target.wrote);
        }
        if (scope.wrote) {
            self.add(scope.workflow,
                     {.target = "memory", .from = detail::render_writes, .to = detail::later_work});
        }

        self.render.reset();
        self.end();
    }
} // namespace rei::gpu::vk
