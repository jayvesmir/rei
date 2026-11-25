#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/descriptor.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/shader.hpp"

namespace rei::gpu::vk {
    struct compute_pipeline_options {
        vk::shader shader;
        std::vector<vk::set_layout> sets{};
        std::uint32_t push_constant_size{};
        std::string name{};
    };

    class compute_pipeline {
      private:
        friend class compute_pass;

        struct state;

        std::shared_ptr<const compute_pipeline::state> m_state;

        explicit compute_pipeline(std::shared_ptr<const compute_pipeline::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::compute_pipeline_options& options)
            -> std::expected<vk::compute_pipeline, rei::error>;
        static auto adopt(const vk::device& device, VkPipeline pipeline, VkPipelineLayout layout)
            -> vk::compute_pipeline;

        [[nodiscard]] auto handle() const noexcept -> VkPipeline;
    };
} // namespace rei::gpu::vk
