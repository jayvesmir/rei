#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
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

    auto compute_pass::_dispatching(std::string_view call, bool indirect) -> bool {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, call, "compute pass") || !self.readable(*self.compute, call)) {
            return false;
        }

        auto& scope = *self.compute;
        const auto writes = scope.pipeline->writes;
        const auto arguments_hidden = indirect && scope.hidden_from_indirect;

        if (writes && !scope.wrote) {
            self.add(scope.workflow,
                     {.target = "memory", .from = detail::earlier_work, .to = detail::compute_work});
        } else if (!scope.concurrent && (writes || scope.last_wrote || arguments_hidden)) {
            self.add(scope.workflow,
                     {
                         .target = "memory",
                         .from = detail::compute_writes,
                         .to = arguments_hidden ? detail::indirect_compute_work : detail::compute_work,
                     });
        }
        self.flush(scope.workflow);

        scope.wrote = scope.wrote || writes;
        scope.last_wrote = writes;
        scope.hidden_from_indirect = writes || (scope.hidden_from_indirect && !indirect);

        return true;
    }

    auto compute_pass::_push_constants(std::span<const std::byte> data) -> void {
        auto& self = *m_commands.m_state;

        if (self.inside(m_open, "push_constants", "compute pass")) {
            self.push_constants(*self.compute, data);
        }
    }

    auto compute_pass::bind(const vk::compute_pipeline& pipeline) -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "bind", "compute pass") ||
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

        if (self.inside(m_open, "bind", "compute pass") &&
            self.usable(descriptors.m_state, "bind in a compute pass", "descriptor set")) {
            self.bind(*self.compute, set, descriptors.m_state);
        }
    }

    auto compute_pass::push_descriptors(std::uint32_t set, std::initializer_list<vk::descriptor> descriptors)
        -> void {
        push_descriptors(set, std::span{descriptors.begin(), descriptors.size()});
    }

    auto compute_pass::push_descriptors(std::uint32_t set, std::span<const vk::descriptor> descriptors)
        -> void {
        auto& self = *m_commands.m_state;

        if (self.inside(m_open, "push_descriptors", "compute pass")) {
            self.push_descriptors(*self.compute, set, descriptors);
        }
    }

    auto compute_pass::dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z)
        -> void {
        if (!_dispatching("dispatch", false)) {
            return;
        }

        const auto& self = *m_commands.m_state;
        self.device->table.vkCmdDispatch(self.pool.buffer, groups_x, groups_y, groups_z);
    }

    auto compute_pass::dispatch_indirect(const vk::buffer& arguments, std::uint64_t offset) -> void {
        auto& self = *m_commands.m_state;

        if (!self.inside(m_open, "dispatch_indirect", "compute pass") ||
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
                            self.compute->label(), detail::quoted(buffer.name, "buffer"),
                            sizeof(VkDispatchIndirectCommand), buffer.size, offset));
            return;
        }

        if (!_dispatching("dispatch_indirect", true)) {
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
