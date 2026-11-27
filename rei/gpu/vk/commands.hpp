#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/buffer.hpp"
#include "rei/gpu/vk/descriptor.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/image.hpp"
#include "rei/gpu/vk/pipeline.hpp"
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

    struct buffer_region {
        std::uint64_t source_offset{};
        std::uint64_t destination_offset{};
        std::optional<std::uint64_t> size{};
    };

    struct image_region {
        std::uint32_t mip{};
        std::uint32_t layer{};
        std::uint64_t buffer_offset{};
    };

    using resolve_target = std::reference_wrapper<const vk::image_view>;

    struct color_target {
        const vk::image_view& target;
        vk::load_op load{vk::load_op::clear};
        vk::store_op store{vk::store_op::store};
        std::array<float, 4> clear{};
        std::optional<vk::resolve_target> resolve{};
    };

    struct depth_target {
        const vk::image_view& target;
        vk::load_op load{vk::load_op::clear};
        vk::store_op store{vk::store_op::store};
        float clear{1.0f};
        bool read_only{false};
    };

    struct render_options {
        std::vector<vk::color_target> color{};
        std::optional<vk::depth_target> depth{};
        std::optional<vk::area> area{};
        std::string name{};
    };

    struct compute_options {
        bool concurrent{false};
        std::string name{};
    };

    struct draw_options {
        std::uint32_t vertices;
        std::uint32_t instances{1};
        std::uint32_t first_vertex{};
        std::uint32_t first_instance{};
    };

    struct draw_indexed_options {
        std::uint32_t indices;
        std::uint32_t instances{1};
        std::uint32_t first_index{};
        std::int32_t vertex_offset{};
        std::uint32_t first_instance{};
    };

    class render_pass {
      private:
        friend class commands;

        vk::commands& m_commands;
        bool m_open{true};

        explicit render_pass(vk::commands& commands) noexcept;

        auto _drawing(std::string_view call) -> bool;
        auto _drawing_indirect(std::string_view call, const vk::buffer& arguments, std::uint64_t offset,
                               std::uint32_t count, std::uint64_t stride) -> bool;
        auto _push_constants(std::span<const std::byte> data) -> void;

      public:
        render_pass(render_pass&& other) = delete;
        ~render_pass();

        auto bind(const vk::graphics_pipeline& pipeline) -> void;
        auto bind(std::uint32_t set, const vk::descriptor_set& descriptors) -> void;
        auto push_descriptors(std::uint32_t set, std::initializer_list<vk::descriptor> descriptors) -> void;
        auto push_descriptors(std::uint32_t set, std::span<const vk::descriptor> descriptors) -> void;

        template <vk::descriptor_block BlockType> auto push_descriptors(const BlockType& block) -> void {
            push_descriptors(BlockType::set, std::span<const vk::descriptor>{block.descriptors()});
        }

        template <typename ValueType>
            requires std::is_trivially_copyable_v<ValueType>
        auto push_constants(const ValueType& value) -> void {
            _push_constants(std::bit_cast<std::array<std::byte, sizeof(ValueType)>>(value));
        }

        auto bind_vertex_buffer(std::uint32_t binding, const vk::buffer& buffer, std::uint64_t offset = 0)
            -> void;
        auto bind_index_buffer(const vk::buffer& buffer, vk::index_type type = vk::index_type::uint32,
                               std::uint64_t offset = 0) -> void;
        auto viewport(const vk::area& area, float min_depth = 0.0f, float max_depth = 1.0f) -> void;
        auto scissor(const vk::area& area) -> void;

        auto draw(const vk::draw_options& options) -> void;
        auto draw_indexed(const vk::draw_indexed_options& options) -> void;
        auto draw_indirect(const vk::buffer& arguments, std::uint64_t offset, std::uint32_t count) -> void;
        auto draw_indexed_indirect(const vk::buffer& arguments, std::uint64_t offset, std::uint32_t count)
            -> void;

        auto end() -> void;
    };

    class compute_pass {
      private:
        friend class commands;

        vk::commands& m_commands;
        bool m_open{true};

        explicit compute_pass(vk::commands& commands) noexcept;

        auto _dispatching(std::string_view call, bool indirect) -> bool;
        auto _push_constants(std::span<const std::byte> data) -> void;

      public:
        compute_pass(compute_pass&& other) = delete;
        ~compute_pass();

        auto bind(const vk::compute_pipeline& pipeline) -> void;
        auto bind(std::uint32_t set, const vk::descriptor_set& descriptors) -> void;
        auto push_descriptors(std::uint32_t set, std::initializer_list<vk::descriptor> descriptors) -> void;
        auto push_descriptors(std::uint32_t set, std::span<const vk::descriptor> descriptors) -> void;

        template <vk::descriptor_block BlockType> auto push_descriptors(const BlockType& block) -> void {
            push_descriptors(BlockType::set, std::span<const vk::descriptor>{block.descriptors()});
        }

        template <typename ValueType>
            requires std::is_trivially_copyable_v<ValueType>
        auto push_constants(const ValueType& value) -> void {
            _push_constants(std::bit_cast<std::array<std::byte, sizeof(ValueType)>>(value));
        }

        auto dispatch(std::uint32_t groups_x, std::uint32_t groups_y = 1, std::uint32_t groups_z = 1) -> void;
        auto dispatch_indirect(const vk::buffer& arguments, std::uint64_t offset = 0) -> void;

        auto end() -> void;
    };

    class commands {
      private:
        friend class compute_pass;
        friend class queue;
        friend class render_pass;
        friend class swapchain;

        struct state;

        std::unique_ptr<commands::state> m_state;

        explicit commands(std::unique_ptr<commands::state> state) noexcept;

      public:
        commands(commands&& other) noexcept;
        auto operator=(commands&& other) noexcept -> commands&;
        ~commands();

        [[nodiscard]] auto render(const vk::render_options& options) -> vk::render_pass;
        [[nodiscard]] auto compute(const vk::compute_options& options = {}) -> vk::compute_pass;

        auto upload(const vk::buffer& destination, std::span<const std::byte> data, std::uint64_t offset = 0)
            -> void;
        auto upload(const vk::image& destination, std::span<const std::byte> pixels,
                    const vk::image_region& region = {}) -> void;
        auto copy(const vk::buffer& source, const vk::buffer& destination,
                  const vk::buffer_region& region = {}) -> void;
        auto copy(const vk::buffer& source, const vk::image& destination, const vk::image_region& region = {})
            -> void;
        auto copy(const vk::image& source, const vk::buffer& destination, const vk::image_region& region = {})
            -> void;
        auto copy(const vk::image& source, const vk::image& destination) -> void;
        auto blit(const vk::image_view& source, const vk::image_view& destination,
                  vk::filter filter = vk::filter::linear) -> void;
        auto generate_mipmaps(const vk::image& image) -> void;

        auto barrier(std::initializer_list<vk::transition> transitions) -> void;
        auto barrier(std::span<const vk::transition> transitions) -> void;

        [[nodiscard]] auto handle() -> VkCommandBuffer;
    };
} // namespace rei::gpu::vk
