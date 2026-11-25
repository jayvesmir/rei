#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/descriptor.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/shader.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    struct vertex_attribute {
        std::uint32_t location;
        vk::format format;
        std::uint32_t offset;
    };

    struct vertex_binding {
        std::uint32_t stride;
        std::vector<vk::vertex_attribute> attributes{};
        bool per_instance{false};
    };

    template <typename ValueType> inline constexpr auto vertex_format = vk::format::undefined;

    template <> inline constexpr auto vertex_format<float> = vk::format::r32_sfloat;

    template <> inline constexpr auto vertex_format<std::array<float, 2>> = vk::format::r32g32_sfloat;

    template <> inline constexpr auto vertex_format<std::array<float, 3>> = vk::format::r32g32b32_sfloat;

    template <> inline constexpr auto vertex_format<std::array<float, 4>> = vk::format::r32g32b32a32_sfloat;

    template <> inline constexpr auto vertex_format<std::uint32_t> = vk::format::r32_uint;

    template <> inline constexpr auto vertex_format<std::array<std::uint8_t, 4>> = vk::format::r8g8b8a8_uint;

    template <typename VertexType, typename... MemberTypes>
        requires(std::is_standard_layout_v<VertexType> && std::is_default_constructible_v<VertexType> &&
                 ((vk::vertex_format<MemberTypes> != vk::format::undefined) && ...))
    auto vertex_layout(MemberTypes VertexType::*... members) -> vk::vertex_binding {
        const auto vertex = VertexType{};
        const auto base = reinterpret_cast<std::uintptr_t>(std::addressof(vertex));

        auto binding = vk::vertex_binding{.stride = sizeof(VertexType)};
        (binding.attributes.push_back({
             .location = static_cast<std::uint32_t>(binding.attributes.size()),
             .format = vk::vertex_format<MemberTypes>,
             .offset = static_cast<std::uint32_t>(
                 reinterpret_cast<std::uintptr_t>(std::addressof(vertex.*members)) - base),
         }),
         ...);

        return binding;
    }

    struct blend {
        vk::blend_factor src_color{vk::blend_factor::one};
        vk::blend_factor dst_color{vk::blend_factor::zero};
        vk::blend_op color_op{vk::blend_op::add};
        vk::blend_factor src_alpha{vk::blend_factor::one};
        vk::blend_factor dst_alpha{vk::blend_factor::zero};
        vk::blend_op alpha_op{vk::blend_op::add};

        static const blend alpha;
        static const blend additive;
    };

    inline constexpr blend blend::alpha{
        .src_color = vk::blend_factor::src_alpha,
        .dst_color = vk::blend_factor::one_minus_src_alpha,
        .dst_alpha = vk::blend_factor::one_minus_src_alpha,
    };

    inline constexpr blend blend::additive{
        .dst_color = vk::blend_factor::one,
        .dst_alpha = vk::blend_factor::one,
    };

    struct color_output {
        vk::format format;
        std::optional<vk::blend> blend{};
    };

    struct depth_output {
        vk::format format;
        bool test{true};
        bool write{true};
        vk::compare_op compare{vk::compare_op::less};
    };

    struct depth_bias {
        float constant{};
        float slope{};
        float clamp{};
    };

    struct graphics_pipeline_options {
        vk::shader vertex;
        std::optional<vk::shader> fragment{};
        std::vector<vk::vertex_binding> vertex_input{};
        std::vector<vk::color_output> color{};
        std::optional<vk::depth_output> depth{};
        std::vector<vk::set_layout> sets{};
        std::uint32_t push_constant_size{};
        vk::topology topology{vk::topology::triangle_list};
        vk::polygon_mode polygon{vk::polygon_mode::fill};
        vk::cull_mode cull{vk::cull_mode::none};
        vk::front_face front{vk::front_face::counter_clockwise};
        std::optional<vk::depth_bias> depth_bias{};
        std::uint32_t samples{1};
        std::string name{};
    };

    class graphics_pipeline {
      private:
        friend class render_pass;

        struct state;

        std::shared_ptr<const graphics_pipeline::state> m_state;

        explicit graphics_pipeline(std::shared_ptr<const graphics_pipeline::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::graphics_pipeline_options& options)
            -> std::expected<vk::graphics_pipeline, rei::error>;
        static auto adopt(const vk::device& device, VkPipeline pipeline, VkPipelineLayout layout)
            -> vk::graphics_pipeline;

        [[nodiscard]] auto handle() const noexcept -> VkPipeline;
    };

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
