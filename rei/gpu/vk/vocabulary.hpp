#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/feature.hpp"
#include "rei/gpu/vk/format.hpp"

namespace rei::gpu::vk {
    enum class buffer_usage : std::uint32_t {
        transfer_src = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        transfer_dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        uniform_buffer = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        storage_buffer = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        index_buffer = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        vertex_buffer = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        indirect_buffer = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        shader_device_address = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };

    enum class image_usage : std::uint32_t {
        transfer_src = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        transfer_dst = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        sampled = VK_IMAGE_USAGE_SAMPLED_BIT,
        storage = VK_IMAGE_USAGE_STORAGE_BIT,
        color_attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        depth_stencil_attachment = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    };

    enum class shader_stage : std::uint32_t {
        vertex = VK_SHADER_STAGE_VERTEX_BIT,
        fragment = VK_SHADER_STAGE_FRAGMENT_BIT,
        compute = VK_SHADER_STAGE_COMPUTE_BIT,
        all = VK_SHADER_STAGE_ALL,
    };

    template <typename EnumType> inline constexpr bool is_flags = false;

    template <> inline constexpr bool is_flags<vk::buffer_usage> = true;

    template <> inline constexpr bool is_flags<vk::image_usage> = true;

    template <> inline constexpr bool is_flags<vk::shader_stage> = true;

    template <typename EnumType>
        requires vk::is_flags<EnumType>
    constexpr auto operator|(EnumType left, EnumType right) noexcept -> EnumType {
        return static_cast<EnumType>(std::to_underlying(left) | std::to_underlying(right));
    }

    template <typename EnumType>
        requires vk::is_flags<EnumType>
    constexpr auto has(EnumType flags, EnumType flag) noexcept -> bool {
        return (std::to_underlying(flags) & std::to_underlying(flag)) == std::to_underlying(flag);
    }

    enum class descriptor_type : std::int32_t {
        sampler = VK_DESCRIPTOR_TYPE_SAMPLER,
        combined_image_sampler = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        sampled_image = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        storage_image = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        uniform_buffer = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        storage_buffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };

    enum class present_mode : std::int32_t {
        immediate = VK_PRESENT_MODE_IMMEDIATE_KHR,
        mailbox = VK_PRESENT_MODE_MAILBOX_KHR,
        fifo = VK_PRESENT_MODE_FIFO_KHR,
        fifo_relaxed = VK_PRESENT_MODE_FIFO_RELAXED_KHR,
    };

    enum class load_op : std::int32_t {
        load = VK_ATTACHMENT_LOAD_OP_LOAD,
        clear = VK_ATTACHMENT_LOAD_OP_CLEAR,
        dont_care = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    };

    enum class store_op : std::int32_t {
        store = VK_ATTACHMENT_STORE_OP_STORE,
        dont_care = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    };

    enum class index_type : std::int32_t {
        uint16 = VK_INDEX_TYPE_UINT16,
        uint32 = VK_INDEX_TYPE_UINT32,
    };

    enum class topology : std::int32_t {
        point_list = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        line_list = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        line_strip = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        triangle_list = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        triangle_strip = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };

    enum class polygon_mode : std::int32_t {
        fill = VK_POLYGON_MODE_FILL,
        line = VK_POLYGON_MODE_LINE,
    };

    enum class cull_mode : std::uint32_t {
        none = VK_CULL_MODE_NONE,
        front = VK_CULL_MODE_FRONT_BIT,
        back = VK_CULL_MODE_BACK_BIT,
    };

    enum class front_face : std::int32_t {
        counter_clockwise = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        clockwise = VK_FRONT_FACE_CLOCKWISE,
    };

    enum class compare_op : std::int32_t {
        never = VK_COMPARE_OP_NEVER,
        less = VK_COMPARE_OP_LESS,
        equal = VK_COMPARE_OP_EQUAL,
        less_or_equal = VK_COMPARE_OP_LESS_OR_EQUAL,
        greater = VK_COMPARE_OP_GREATER,
        not_equal = VK_COMPARE_OP_NOT_EQUAL,
        greater_or_equal = VK_COMPARE_OP_GREATER_OR_EQUAL,
        always = VK_COMPARE_OP_ALWAYS,
    };

    enum class blend_factor : std::int32_t {
        zero = VK_BLEND_FACTOR_ZERO,
        one = VK_BLEND_FACTOR_ONE,
        src_color = VK_BLEND_FACTOR_SRC_COLOR,
        one_minus_src_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        dst_color = VK_BLEND_FACTOR_DST_COLOR,
        one_minus_dst_color = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        src_alpha = VK_BLEND_FACTOR_SRC_ALPHA,
        one_minus_src_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        dst_alpha = VK_BLEND_FACTOR_DST_ALPHA,
        one_minus_dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        constant_color = VK_BLEND_FACTOR_CONSTANT_COLOR,
        one_minus_constant_color = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        constant_alpha = VK_BLEND_FACTOR_CONSTANT_ALPHA,
        one_minus_constant_alpha = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        src_alpha_saturate = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        src1_color = VK_BLEND_FACTOR_SRC1_COLOR,
        one_minus_src1_color = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
        src1_alpha = VK_BLEND_FACTOR_SRC1_ALPHA,
        one_minus_src1_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
    };

    enum class blend_op : std::int32_t {
        add = VK_BLEND_OP_ADD,
        subtract = VK_BLEND_OP_SUBTRACT,
        reverse_subtract = VK_BLEND_OP_REVERSE_SUBTRACT,
        min = VK_BLEND_OP_MIN,
        max = VK_BLEND_OP_MAX,
    };

    enum class filter : std::int32_t {
        nearest = VK_FILTER_NEAREST,
        linear = VK_FILTER_LINEAR,
    };

    enum class address_mode : std::int32_t {
        repeat = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        mirrored_repeat = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        clamp_to_edge = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        clamp_to_border = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    };

    enum class device_type : std::int32_t {
        other = VK_PHYSICAL_DEVICE_TYPE_OTHER,
        integrated_gpu = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        discrete_gpu = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
        virtual_gpu = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
        cpu = VK_PHYSICAL_DEVICE_TYPE_CPU,
    };

    enum class access : std::uint8_t {
        undefined,
        shader_read,
        shader_write,
        color_attachment,
        depth_attachment,
        depth_read,
        transfer_src,
        transfer_dst,
        present,
        manual,
    };

    enum class host_access : std::uint8_t { none, write, read_write };
    enum class severity : std::uint8_t { verbose, info, warning, error };
    enum class validation : std::uint8_t { none, core, full };

    struct extent {
        std::uint32_t width{1};
        std::uint32_t height{1};
        std::uint32_t depth{1};
    };

    struct area {
        std::int32_t x{};
        std::int32_t y{};
        std::uint32_t width;
        std::uint32_t height;
    };

    [[nodiscard]] auto to_string(vk::feature feature) noexcept -> std::string_view;
    [[nodiscard]] auto to_feature(std::string_view name) noexcept -> std::optional<vk::feature>;
} // namespace rei::gpu::vk
