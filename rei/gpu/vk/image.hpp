#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    struct image_view_options {
        std::uint32_t first_mip{};
        std::optional<std::uint32_t> mip_levels{};
        std::uint32_t first_layer{};
        std::optional<std::uint32_t> array_layers{};
        std::string name{};
    };

    class image_view {
      private:
        friend class commands;
        friend class image;

        struct state;

        std::shared_ptr<image_view::state> m_state;

        explicit image_view(std::shared_ptr<image_view::state> state) noexcept;

      public:
        static auto make(const vk::image& image, const vk::image_view_options& options = {})
            -> std::expected<vk::image_view, rei::error>;

        image_view(image_view&& other) noexcept = default;
        auto operator=(image_view&& other) noexcept -> image_view& = default;

        [[nodiscard]] auto format() const noexcept -> vk::format;
        [[nodiscard]] auto extent() const noexcept -> vk::extent;
        [[nodiscard]] auto handle() const noexcept -> VkImageView;
    };

    struct image_options {
        vk::format format;
        vk::extent extent;
        vk::image_usage usage;
        std::uint32_t mip_levels{1};
        std::uint32_t array_layers{1};
        std::uint32_t samples{1};
        bool cube{false};
        std::optional<vk::access> home{};
        std::string name{};
    };

    class image {
      private:
        friend class commands;
        friend class device;
        friend class image_view;
        friend class queue;

        struct state;

        std::shared_ptr<image::state> m_state;
        vk::image_view m_view;

        image(std::shared_ptr<image::state> state, vk::image_view view) noexcept;

        static auto _view(const std::shared_ptr<image::state>& image, const VkImageSubresourceRange& range,
                          std::string name, bool own) -> std::expected<vk::image_view, rei::error>;
        static auto _complete(const vk::image_options& options, vk::access home,
                              std::shared_ptr<image::state> state) -> std::expected<vk::image, rei::error>;

      public:
        static auto make(const vk::device& device, const vk::image_options& options)
            -> std::expected<vk::image, rei::error>;
        static auto adopt(const vk::device& device, VkImage handle, VkDeviceMemory memory,
                          const vk::image_options& options) -> std::expected<vk::image, rei::error>;

        image(image&& other) noexcept = default;
        auto operator=(image&& other) noexcept -> image& = default;

        operator const vk::image_view&() const noexcept;

        [[nodiscard]] auto view() const noexcept -> const vk::image_view&;
        [[nodiscard]] auto format() const noexcept -> vk::format;
        [[nodiscard]] auto extent() const noexcept -> vk::extent;
        [[nodiscard]] auto home() const noexcept -> vk::access;
        [[nodiscard]] auto handle() const noexcept -> VkImage;
    };

    [[nodiscard]] auto mip_count(vk::extent extent) noexcept -> std::uint32_t;

    struct sampler_options {
        vk::filter mag{vk::filter::linear};
        vk::filter min{vk::filter::linear};
        vk::filter mip{vk::filter::linear};
        vk::address_mode address{vk::address_mode::repeat};
        std::optional<float> anisotropy{};
        std::optional<vk::compare_op> compare{};
        std::string name{};
    };

    class sampler {
      private:
        struct state;

        std::shared_ptr<const sampler::state> m_state;

        explicit sampler(std::shared_ptr<const sampler::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::sampler_options& options = {})
            -> std::expected<vk::sampler, rei::error>;

        [[nodiscard]] auto handle() const noexcept -> VkSampler;
    };
} // namespace rei::gpu::vk
