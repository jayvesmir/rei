#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/image.hpp"
#include "rei/gpu/vk/instance.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    struct swapchain_options {
        vk::surface surface;
        vk::extent extent;
        vk::present_mode present_mode{vk::present_mode::fifo};
        std::optional<vk::format> format{};
        vk::image_usage usage{vk::image_usage::color_attachment};
        std::string name{};
    };

    class frame {
      private:
        friend class swapchain;

        struct state;

        std::unique_ptr<frame::state> m_state;

        explicit frame(std::unique_ptr<frame::state> state) noexcept;

      public:
        frame(frame&& other) noexcept;
        auto operator=(frame&& other) noexcept -> frame&;
        ~frame();

        [[nodiscard]] auto image() const noexcept -> const vk::image&;
        [[nodiscard]] auto index() const noexcept -> std::uint32_t;
    };

    class swapchain {
      private:
        friend class commands;
        friend class frame;
        friend class image;

        struct generation;
        struct state;

        std::unique_ptr<swapchain::state> m_state;

        explicit swapchain(std::unique_ptr<swapchain::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::swapchain_options& options)
            -> std::expected<vk::swapchain, rei::error>;

        swapchain(swapchain&& other) noexcept;
        auto operator=(swapchain&& other) noexcept -> swapchain&;
        ~swapchain();

        auto acquire(vk::extent extent) -> std::expected<vk::frame, rei::error>;
        auto present(vk::frame frame) -> std::expected<void, rei::error>;

        [[nodiscard]] auto present_mode() const noexcept -> vk::present_mode;
        auto present_mode(vk::present_mode mode) noexcept -> void;

        [[nodiscard]] auto format() const noexcept -> vk::format;
        [[nodiscard]] auto extent() const noexcept -> vk::extent;
        [[nodiscard]] auto handle() const noexcept -> VkSwapchainKHR;
    };
} // namespace rei::gpu::vk
