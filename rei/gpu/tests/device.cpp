#include <format>

#include <vulkan/vulkan_core.h>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/gpu/tests/shaders/lanes.hpp"
#include "rei/gpu/tests/shaders/numbered.hpp"
#include "rei/gpu/tests/support.hpp"
#include "rei/gpu/vk.hpp"

namespace rei::gpu::tests {
    TEST_F(vulkan, a_device_has_what_was_required_and_what_it_supports_of_what_was_preferred) {
        const auto options = vk::device_options{
            .require = {vk::feature::sampler_anisotropy},
            .prefer = {vk::feature::unified_image_layouts_khr, vk::feature::invocation_mask_huawei},
            .name = "preferring",
        };

        const auto made = vk::device::make(instance(), options);
        ASSERT_TRUE(made) << made.error();

        EXPECT_TRUE(made->has(vk::feature::sampler_anisotropy));
        EXPECT_TRUE(made->has(vk::feature::dynamic_rendering));
        EXPECT_FALSE(made->has(vk::feature::invocation_mask_huawei));
        EXPECT_FALSE(made->has(vk::feature::geometry_shader));

        EXPECT_FALSE(made->physical().name.empty());
        EXPECT_GT(made->limits().maxImageDimension2D, 0U);
        EXPECT_NE(made->queue().handle(), VK_NULL_HANDLE);
    }

    TEST_F(vulkan, a_feature_that_two_extensions_offer_is_enabled_only_through_those_the_device_has) {
        const auto options = vk::device_options{
            .prefer =
                {
                    vk::feature::compute_derivative_group_quads_khr,
                    vk::feature::compute_derivative_group_quads_nv,
                },
        };

        const auto made = vk::device::make(instance(), options);
        ASSERT_TRUE(made) << made.error();
    }

    TEST_F(vulkan, a_device_that_lacks_what_was_required_is_refused_and_the_error_names_what) {
        const auto physical = device().physical();
        const auto missing = vk::feature::invocation_mask_huawei;

        EXPECT_TRUE(has_error(vk::device::make(instance(), {.require = {missing}}),
                              vk::error_code::no_suitable_device,
                              std::format("'{}' lacks invocation_mask_huawei", physical.name)));
        EXPECT_TRUE(has_error(vk::device::make(instance(), {.require = {missing}, .physical = physical}),
                              vk::error_code::missing_feature, "lacks invocation_mask_huawei"));
        EXPECT_TRUE(
            has_error(vk::device::make(instance(), {.extensions = {"VK_REI_missing"}, .physical = physical}),
                      vk::error_code::missing_extension, "lacks the extension VK_REI_missing"));
    }

    TEST_F(vulkan,
           a_pipeline_whose_shader_needs_a_feature_the_device_has_not_enabled_is_refused_with_its_name) {
        const auto numbered = shaders::numbered::vertex();
        const auto lanes = shaders::lanes::compute();
        ASSERT_TRUE(numbered) << numbered.error();
        ASSERT_TRUE(lanes) << lanes.error();

        EXPECT_TRUE(
            has_error(vk::graphics_pipeline::make(device(), {.vertex = *numbered, .name = "numbered"}),
                      vk::error_code::missing_feature,
                      "graphics pipeline 'numbered': the vertex shader declares DrawParameters, which needs "
                      "the feature shader_draw_parameters or the extension VK_KHR_shader_draw_parameters"));

        const auto drawing = vk::device::make(instance(), {.require = {vk::feature::shader_draw_parameters}});
        ASSERT_TRUE(drawing) << drawing.error();
        const auto made = vk::graphics_pipeline::make(*drawing, {.vertex = *numbered, .name = "numbered"});
        EXPECT_TRUE(made) << made.error();

        const auto layout =
            vk::set_layout::make(device(), {.bindings = shaders::lanes::set_0::bindings(), .pushed = true});
        ASSERT_TRUE(layout) << layout.error();
        const auto counted = vk::compute_pipeline::make(device(), {.shader = *lanes, .sets = {*layout}});
        EXPECT_TRUE(counted) << counted.error();
    }
} // namespace rei::gpu::tests
