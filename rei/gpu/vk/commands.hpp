#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/buffer.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/image.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    class resource {
      private:
        friend class commands;

        std::optional<std::reference_wrapper<const vk::buffer>> m_buffer;
        std::optional<std::reference_wrapper<const vk::image_view>> m_view;

      public:
        resource(const vk::buffer& buffer) noexcept : m_buffer(buffer) {}
        resource(const vk::image& image) noexcept : m_view(image.view()) {}
        resource(const vk::image_view& view) noexcept : m_view(view) {}
    };

    struct transition {
        vk::resource target;
        vk::access from;
        vk::access to;
    };

    class commands {
      private:
        friend class queue;

        struct state;

        std::unique_ptr<commands::state> m_state;

        explicit commands(std::unique_ptr<commands::state> state) noexcept;

      public:
        commands(commands&& other) noexcept;
        auto operator=(commands&& other) noexcept -> commands&;
        ~commands();

        auto barrier(std::initializer_list<vk::transition> transitions) -> void;
        auto barrier(std::span<const vk::transition> transitions) -> void;

        [[nodiscard]] auto handle() -> VkCommandBuffer;
    };
} // namespace rei::gpu::vk
