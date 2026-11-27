#include <optional>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "gtest/gtest.h"

#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::tests {
    namespace {
        template <typename LeftType, typename RightType>
        concept combinable = requires(LeftType left, RightType right) { left | right; };
    } // namespace

    static_assert(std::to_underlying(vk::format::r8g8b8a8_srgb) == VK_FORMAT_R8G8B8A8_SRGB);
    static_assert(vk::has(vk::image_usage::sampled | vk::image_usage::storage, vk::image_usage::storage));
    static_assert(!vk::has(vk::image_usage::sampled, vk::image_usage::storage));
    static_assert(!combinable<vk::image_usage, vk::buffer_usage>);
    static_assert(!combinable<vk::filter, vk::filter>);

    TEST(vulkan_vocabulary, a_feature_is_found_by_its_name_or_by_the_name_it_was_promoted_from) {
        EXPECT_EQ(vk::to_string(vk::feature::sampler_anisotropy), "sampler_anisotropy");
        EXPECT_EQ(vk::to_string(vk::feature::mesh_shader_ext), "mesh_shader_ext");

        EXPECT_EQ(vk::to_feature("mesh_shader_nv"), vk::feature::mesh_shader_nv);
        EXPECT_EQ(vk::to_feature("timeline_semaphore_khr"), vk::feature::timeline_semaphore);
        EXPECT_EQ(vk::to_feature("mesh_shader"), std::nullopt);
    }
} // namespace rei::gpu::tests
