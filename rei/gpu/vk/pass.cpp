#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/commands.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    auto commands::state::inside(bool open, std::string_view call, std::string_view pass) -> bool {
        if (!open) {
            fail(vk::error_code::invalid_argument, std::format("{} in a {} that has ended", call, pass));
        }

        return !failure;
    }

    auto commands::state::bind(state::scope& scope, std::uint32_t set,
                               const std::shared_ptr<vk::descriptor_set::state>& descriptors) -> void {
        const auto& bound = *descriptors;

        if (!scope.pipeline) {
            fail(vk::error_code::invalid_argument,
                 std::format("bind of {} in {}: no pipeline is bound",
                             detail::quoted(bound.name, "descriptor set"), scope.label()));
            return;
        }

        const auto& pipeline = *scope.pipeline;
        if (!pipeline.adopted) {
            if (set >= pipeline.sets.size()) {
                fail(vk::error_code::invalid_argument,
                     std::format("bind of {} in {}: set {} isn't in {}, which has {}",
                                 detail::quoted(bound.name, "descriptor set"), scope.label(), set,
                                 detail::quoted(pipeline.name, pipeline.kind), pipeline.sets.size()));
                return;
            }

            const auto& expected = pipeline.sets[set].m_state;
            if (bound.layout != expected &&
                (expected->pushed || bound.layout->bindings != expected->bindings)) {
                fail(vk::error_code::invalid_argument,
                     std::format("bind of {} in {}: it was made with {}, and set {} of {} has {}",
                                 detail::quoted(bound.name, "descriptor set"), scope.label(),
                                 detail::quoted(bound.layout->name, "set layout"), set,
                                 detail::quoted(pipeline.name, pipeline.kind),
                                 detail::quoted(expected->name, "set layout")));
                return;
            }

            scope.sets[set] = {.allocated = descriptors};
        }

        if (std::ranges::any_of(bound.references, [](const detail::reference& reference) -> bool {
                return reference.resource.expired();
            })) {
            const auto lock = std::scoped_lock{device->mutex};
            for (auto& item : device->retired) {
                item.serial = std::max(item.serial, serial);
            }
        }

        device->table.vkCmdBindDescriptorSets(pool.buffer, scope.bind_point, pipeline.layout, set, 1,
                                              &bound.handle, 0, nullptr);
    }

    auto commands::state::push_descriptors(state::scope& scope, std::uint32_t set,
                                           std::span<const vk::descriptor> descriptors) -> void {
        if (!scope.pipeline || scope.pipeline->adopted) {
            fail(vk::error_code::invalid_argument,
                 std::format("push_descriptors in {}: {}", scope.label(),
                             scope.pipeline ? "an adopted pipeline takes them through raw calls"
                                            : "no pipeline is bound"));
            return;
        }

        const auto& pipeline = *scope.pipeline;
        if (set >= pipeline.sets.size() || !pipeline.sets[set].m_state->pushed) {
            fail(
                vk::error_code::invalid_argument,
                std::format("push_descriptors in {}: set {} of {} {}", scope.label(), set,
                            detail::quoted(pipeline.name, pipeline.kind),
                            set < pipeline.sets.size() ? "has a layout that isn't pushed" : "doesn't exist"));
            return;
        }

        const auto resolved = pipeline.sets[set].m_state->resolve(descriptors, VK_NULL_HANDLE);
        if (!resolved) {
            fail(vk::error_code::invalid_argument,
                 std::format("push_descriptors in {}: {}", scope.label(), resolved.error()));
            return;
        }

        scope.sets[set] = {.pushed = true};
        device->table.vkCmdPushDescriptorSet(pool.buffer, scope.bind_point, pipeline.layout, set,
                                             static_cast<std::uint32_t>(resolved->writes.size()),
                                             resolved->writes.data());
    }

    auto commands::state::push_constants(const state::scope& scope, std::span<const std::byte> data) -> void {
        if (!scope.pipeline || scope.pipeline->adopted) {
            fail(vk::error_code::invalid_argument,
                 std::format("push_constants in {}: {}", scope.label(),
                             scope.pipeline ? "an adopted pipeline takes them through raw calls"
                                            : "no pipeline is bound"));
            return;
        }

        const auto& pipeline = *scope.pipeline;
        if (data.size() != pipeline.push_constant_size) {
            fail(vk::error_code::invalid_argument,
                 std::format("push_constants in {}: {} bytes given, and the push_constant_size of {} is {}",
                             scope.label(), data.size(), detail::quoted(pipeline.name, pipeline.kind),
                             pipeline.push_constant_size));
            return;
        }

        device->table.vkCmdPushConstants(pool.buffer, pipeline.layout, VK_SHADER_STAGE_ALL, 0,
                                         pipeline.push_constant_size, data.data());
    }

    auto commands::state::readable(const state::scope& scope, std::string_view call) -> bool {
        if (!scope.pipeline) {
            fail(vk::error_code::invalid_argument,
                 std::format("{} in {}: no pipeline is bound", call, scope.label()));
            return false;
        }

        const auto& pipeline = *scope.pipeline;
        for (auto index = 0uz; index < pipeline.reads.size(); index++) {
            const auto& reads = pipeline.reads[index];
            const auto& bound = scope.sets[index];

            if (!reads.empty() && !bound.pushed && !bound.allocated) {
                fail(vk::error_code::invalid_argument,
                     std::format("{} in {}: {} reads set {}, and nothing is bound or pushed there", call,
                                 scope.label(), detail::quoted(pipeline.name, pipeline.kind), index));
                return false;
            }
            if (!bound.allocated) {
                continue;
            }

            for (const auto& reference : bound.allocated->references) {
                if (std::ranges::contains(reads, reference.binding) && reference.resource.expired()) {
                    fail(vk::error_code::invalid_argument,
                         std::format("{} in {}: {} binding {} refers to {}, which was destroyed", call,
                                     scope.label(), detail::quoted(bound.allocated->name, "descriptor set"),
                                     reference.binding, reference.target));
                    return false;
                }
            }
        }

        return true;
    }
} // namespace rei::gpu::vk
