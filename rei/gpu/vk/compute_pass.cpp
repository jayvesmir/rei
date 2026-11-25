#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    compute_pass::compute_pass(vk::commands& commands) noexcept : m_commands(commands) {}

    compute_pass::~compute_pass() {
        end();
    }

    auto compute_pass::_recording(std::string_view call) -> bool {
        auto& self = *m_commands.m_state;

        if (!m_open) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{} in a compute pass that has ended", call));
        }

        return !self.failure;
    }

    auto compute_pass::_dispatching(std::string_view call) -> bool {
        if (!_recording(call)) {
            return false;
        }

        auto& self = *m_commands.m_state;
        auto& scope = *self.compute;

        if (!scope.pipeline) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("{} in {}: no pipeline is bound", call, scope.label));
            return false;
        }

        const auto& pipeline = *scope.pipeline;
        for (auto index = 0uz; index < pipeline.reads.size(); index++) {
            const auto& reads = pipeline.reads[index];
            const auto& bound = scope.sets[index];

            if (!reads.empty() && !bound.pushed && !bound.allocated) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("{} in {}: {} reads set {}, and nothing is bound or pushed there", call,
                                      scope.label, detail::quoted(pipeline.name, "compute pipeline"), index));
                return false;
            }
            if (!bound.allocated) {
                continue;
            }

            for (const auto& reference : bound.allocated->references) {
                if (std::ranges::contains(reads, reference.binding) && reference.resource.expired()) {
                    self.fail(vk::error_code::invalid_argument,
                              std::format("{} in {}: {} binding {} refers to {}, which was destroyed", call,
                                          scope.label,
                                          detail::quoted(bound.allocated->name, "descriptor set"),
                                          reference.binding, reference.target));
                    return false;
                }
            }
        }

        if (pipeline.writes && !scope.wrote) {
            self.add(scope.workflow,
                     {.target = "memory", .from = detail::earlier_work, .to = detail::compute_work});
        } else if (!scope.concurrent && (pipeline.writes || scope.last_wrote)) {
            self.add(scope.workflow,
                     {.target = "memory", .from = detail::compute_writes, .to = detail::compute_work});
        }
        self.flush(scope.workflow);

        scope.wrote = scope.wrote || pipeline.writes;
        scope.last_wrote = pipeline.writes;

        return true;
    }

    auto compute_pass::_push_constants(std::span<const std::byte> data) -> void {
        if (!_recording("push_constants")) {
            return;
        }

        auto& self = *m_commands.m_state;
        const auto& scope = *self.compute;

        if (!scope.pipeline || scope.pipeline->adopted) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("push_constants in {}: {}", scope.label,
                                  scope.pipeline ? "an adopted pipeline takes them through raw calls"
                                                 : "no pipeline is bound"));
            return;
        }

        const auto& pipeline = *scope.pipeline;
        if (data.size() != pipeline.push_constant_size) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("push_constants in {}: {} bytes given, and the push_constant_size of {} is {}",
                            scope.label, data.size(), detail::quoted(pipeline.name, "compute pipeline"),
                            pipeline.push_constant_size));
            return;
        }

        self.device->table.vkCmdPushConstants(self.pool.buffer, pipeline.layout, VK_SHADER_STAGE_ALL, 0,
                                              pipeline.push_constant_size, data.data());
    }

    auto compute_pass::bind(const vk::compute_pipeline& pipeline) -> void {
        auto& self = *m_commands.m_state;

        if (!_recording("bind") ||
            !self.usable(pipeline.m_state, "bind in a compute pass", "compute pipeline")) {
            return;
        }

        auto& scope = *self.compute;
        scope.pipeline = pipeline.m_state;
        scope.sets.resize(std::max(scope.sets.size(), scope.pipeline->sets.size()));

        self.device->table.vkCmdBindPipeline(self.pool.buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                             scope.pipeline->handle);
    }

    auto compute_pass::bind(std::uint32_t set, const vk::descriptor_set& descriptors) -> void {
        auto& self = *m_commands.m_state;

        if (!_recording("bind") ||
            !self.usable(descriptors.m_state, "bind in a compute pass", "descriptor set")) {
            return;
        }

        auto& scope = *self.compute;
        const auto& bound = *descriptors.m_state;

        if (!scope.pipeline) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("bind of {} in {}: no pipeline is bound",
                                  detail::quoted(bound.name, "descriptor set"), scope.label));
            return;
        }

        const auto& pipeline = *scope.pipeline;
        if (!pipeline.adopted) {
            if (set >= pipeline.sets.size()) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("bind of {} in {}: set {} isn't in {}, which has {}",
                                      detail::quoted(bound.name, "descriptor set"), scope.label, set,
                                      detail::quoted(pipeline.name, "compute pipeline"),
                                      pipeline.sets.size()));
                return;
            }

            const auto& expected = pipeline.sets[set].m_state;
            if (bound.layout != expected &&
                (expected->pushed || bound.layout->bindings != expected->bindings)) {
                self.fail(vk::error_code::invalid_argument,
                          std::format("bind of {} in {}: it was made with {}, and set {} of {} has {}",
                                      detail::quoted(bound.name, "descriptor set"), scope.label,
                                      detail::quoted(bound.layout->name, "set layout"), set,
                                      detail::quoted(pipeline.name, "compute pipeline"),
                                      detail::quoted(expected->name, "set layout")));
                return;
            }

            scope.sets[set] = {.allocated = descriptors.m_state};
        }

        if (std::ranges::any_of(bound.references, [](const detail::reference& reference) -> bool {
                return reference.resource.expired();
            })) {
            const auto lock = std::scoped_lock{self.device->mutex};
            for (auto& item : self.device->retired) {
                item.serial = std::max(item.serial, self.serial);
            }
        }

        self.device->table.vkCmdBindDescriptorSets(self.pool.buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                   pipeline.layout, set, 1, &bound.handle, 0, nullptr);
    }

    auto compute_pass::push_descriptors(std::uint32_t set, std::initializer_list<vk::descriptor> descriptors)
        -> void {
        push_descriptors(set, std::span{descriptors.begin(), descriptors.size()});
    }

    auto compute_pass::push_descriptors(std::uint32_t set, std::span<const vk::descriptor> descriptors)
        -> void {
        if (!_recording("push_descriptors")) {
            return;
        }

        auto& self = *m_commands.m_state;
        auto& scope = *self.compute;

        if (!scope.pipeline || scope.pipeline->adopted) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("push_descriptors in {}: {}", scope.label,
                                  scope.pipeline ? "an adopted pipeline takes them through raw calls"
                                                 : "no pipeline is bound"));
            return;
        }

        const auto& pipeline = *scope.pipeline;
        if (set >= pipeline.sets.size() || !pipeline.sets[set].m_state->pushed) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("push_descriptors in {}: set {} of {} {}", scope.label, set,
                            detail::quoted(pipeline.name, "compute pipeline"),
                            set < pipeline.sets.size() ? "has a layout that isn't pushed" : "doesn't exist"));
            return;
        }

        const auto resolved = pipeline.sets[set].m_state->resolve(descriptors, VK_NULL_HANDLE);
        if (!resolved) {
            self.fail(vk::error_code::invalid_argument,
                      std::format("push_descriptors in {}: {}", scope.label, resolved.error()));
            return;
        }

        scope.sets[set] = {.pushed = true};
        self.device->table.vkCmdPushDescriptorSet(
            self.pool.buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, set,
            static_cast<std::uint32_t>(resolved->writes.size()), resolved->writes.data());
    }

    auto compute_pass::dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z)
        -> void {
        if (!_dispatching("dispatch")) {
            return;
        }

        const auto& self = *m_commands.m_state;
        self.device->table.vkCmdDispatch(self.pool.buffer, groups_x, groups_y, groups_z);
    }

    auto compute_pass::dispatch_indirect(const vk::buffer& arguments, std::uint64_t offset) -> void {
        auto& self = *m_commands.m_state;

        if (!_recording("dispatch_indirect") ||
            !self.usable(arguments.m_state, "dispatch_indirect in a compute pass", "buffer")) {
            return;
        }

        const auto& buffer = *arguments.m_state;
        if (!vk::has(buffer.usage, vk::buffer_usage::indirect_buffer) || offset % 4 != 0 ||
            !detail::fits(buffer.size, offset, sizeof(VkDispatchIndirectCommand))) {
            self.fail(
                vk::error_code::invalid_argument,
                std::format("dispatch_indirect in {}: {} needs the indirect_buffer usage and {} bytes at "
                            "an offset that is a multiple of 4, and it has {} at offset {}",
                            self.compute->label, detail::quoted(buffer.name, "buffer"),
                            sizeof(VkDispatchIndirectCommand), buffer.size, offset));
            return;
        }

        if (!_dispatching("dispatch_indirect")) {
            return;
        }

        self.device->table.vkCmdDispatchIndirect(self.pool.buffer, buffer.handle, offset);
    }

    auto compute_pass::end() -> void {
        if (!std::exchange(m_open, false) || !m_commands.m_state || m_commands.m_state->failure) {
            return;
        }

        auto& self = *m_commands.m_state;
        if (self.compute->wrote) {
            self.add(self.compute->workflow,
                     {.target = "memory", .from = detail::compute_writes, .to = detail::later_work});
        }

        self.compute.reset();
        self.end();
    }
} // namespace rei::gpu::vk
