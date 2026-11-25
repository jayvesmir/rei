#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    class shader {
      private:
        friend class compute_pipeline;
        friend class set_layout;

        struct state;

        std::shared_ptr<const shader::state> m_state;

        explicit shader(std::shared_ptr<const shader::state> state) noexcept;

      public:
        static auto make(std::span<const std::uint32_t> spirv, std::string_view entry_point = {})
            -> std::expected<vk::shader, rei::error>;
        static auto load(const std::filesystem::path& file, std::string_view entry_point = {})
            -> std::expected<vk::shader, rei::error>;

        [[nodiscard]] auto stage() const noexcept -> vk::shader_stage;
        [[nodiscard]] auto entry_point() const noexcept -> std::string_view;
        [[nodiscard]] auto local_size() const noexcept -> vk::extent;
    };
} // namespace rei::gpu::vk
