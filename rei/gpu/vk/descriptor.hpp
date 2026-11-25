#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/buffer.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/image.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    struct set_binding {
        std::uint32_t binding;
        vk::descriptor_type type;
        vk::shader_stage stages{vk::shader_stage::all};
        std::uint32_t count{1};

        auto operator==(const set_binding& other) const noexcept -> bool = default;
    };

    struct set_layout_options {
        std::vector<vk::set_binding> bindings{};
        bool pushed{false};
        std::string name{};
    };

    class set_layout {
      private:
        friend class commands;
        friend class compute_pipeline;
        friend class descriptor_set;
        friend class graphics_pipeline;

        struct state;

        std::shared_ptr<const set_layout::state> m_state;

        explicit set_layout(std::shared_ptr<const set_layout::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::set_layout_options& options)
            -> std::expected<vk::set_layout, rei::error>;

        [[nodiscard]] auto handle() const noexcept -> VkDescriptorSetLayout;
    };

    class descriptor {
      private:
        friend class descriptor_set;
        friend class set_layout;

        std::uint32_t m_binding;
        std::uint32_t m_element{};
        std::optional<std::reference_wrapper<const vk::buffer>> m_buffer;
        std::uint64_t m_offset{};
        std::optional<std::uint64_t> m_size;
        std::optional<std::reference_wrapper<const vk::image_view>> m_view;
        std::optional<std::reference_wrapper<const vk::sampler>> m_sampler;

      public:
        descriptor(std::uint32_t binding, const vk::buffer& buffer) noexcept
            : m_binding(binding), m_buffer(buffer) {}

        descriptor(std::uint32_t binding, const vk::buffer& buffer, std::uint64_t offset,
                   std::uint64_t size) noexcept
            : m_binding(binding), m_buffer(buffer), m_offset(offset), m_size(size) {}

        descriptor(std::uint32_t binding, const vk::image_view& view) noexcept
            : m_binding(binding), m_view(view) {}

        descriptor(std::uint32_t binding, const vk::image_view& view, const vk::sampler& sampler) noexcept
            : m_binding(binding), m_view(view), m_sampler(sampler) {}

        descriptor(std::uint32_t binding, const vk::sampler& sampler) noexcept
            : m_binding(binding), m_sampler(sampler) {}

        [[nodiscard]] auto at(std::uint32_t element) const noexcept -> vk::descriptor {
            auto copy = *this;
            copy.m_element = element;

            return copy;
        }
    };

    struct descriptor_set_options {
        vk::set_layout layout;
        std::string name{};
    };

    class descriptor_set {
      private:
        friend class commands;
        friend class compute_pass;

        struct state;

        std::shared_ptr<descriptor_set::state> m_state;

        explicit descriptor_set(std::shared_ptr<descriptor_set::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::descriptor_set_options& options)
            -> std::expected<vk::descriptor_set, rei::error>;

        descriptor_set(descriptor_set&& other) noexcept = default;
        auto operator=(descriptor_set&& other) noexcept -> descriptor_set& = default;

        auto write(std::initializer_list<vk::descriptor> descriptors) -> std::expected<void, rei::error>;
        auto write(std::span<const vk::descriptor> descriptors) -> std::expected<void, rei::error>;

        [[nodiscard]] auto handle() const noexcept -> VkDescriptorSet;
    };
} // namespace rei::gpu::vk
