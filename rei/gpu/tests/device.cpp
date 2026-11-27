#include <format>

#include <vulkan/vulkan_core.h>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
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
} // namespace rei::gpu::tests
