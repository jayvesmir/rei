#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/gpu/tests/support.hpp"
#include "rei/gpu/vk.hpp"

namespace rei::gpu::tests {
    namespace {
        constexpr auto square = vk::extent{.width = 64, .height = 64};
        constexpr auto wide = vk::extent{.width = 128, .height = 96};
    } // namespace

    TEST_F(vulkan, a_frame_is_drawn_read_back_and_presented_behind_the_waits_rei_owes) {
        if (!surface()) {
            GTEST_SKIP() << "no driver here has a headless surface";
        }

        const auto queue = device().queue();
        auto swapchain = vk::swapchain::make(
            device(), {
                          .surface = *surface(),
                          .extent = square,
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .name = "main",
                      });
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 64 * 64 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        ASSERT_TRUE(swapchain) << swapchain.error();
        ASSERT_TRUE(readback) << readback.error();
        EXPECT_NE(swapchain->handle(), VK_NULL_HANDLE);
        EXPECT_EQ(swapchain->extent().width, 64U);
        EXPECT_EQ(swapchain->present_mode(), vk::present_mode::fifo);

        auto frame = swapchain->acquire(square);
        ASSERT_TRUE(frame) << frame.error();
        EXPECT_EQ(frame->image().home(), vk::access::present);
        EXPECT_EQ(frame->image().format(), swapchain->format());
        EXPECT_EQ(frame->image().extent().height, 64U);

        const auto image = std::format("main image {}", frame->index());
        const auto built = take_trace();
        ASSERT_EQ(built.size(), 1U);
        EXPECT_TRUE(built[0].starts_with("swapchain main: ")) << built[0];
        EXPECT_TRUE(built[0].ends_with(" at 64x64, fifo")) << built[0];

        auto drawing = queue.record("draw");
        drawing.render({.color = {{.target = frame->image(), .clear = {0.0f, 1.0f, 0.0f, 1.0f}}}}).end();
        ASSERT_TRUE(queue.submit(std::move(drawing)));

        auto reading = queue.record("read");
        reading.copy(frame->image(), *readback);
        const auto read = queue.submit(std::move(reading));
        ASSERT_TRUE(read) << read.error();

        const auto presented = swapchain->present(std::move(*frame));
        ASSERT_TRUE(presented) << presented.error();
        ASSERT_TRUE(device().wait(*read));

        EXPECT_EQ(
            take_trace(),
            (std::vector<std::string>{
                std::format("draw: render: barrier {}: present → color_attachment, discard", image),
                std::format("draw: end: barrier {}: color_attachment → present", image),
                std::format("submit draw: first use: {0} undefined → present; waits for {0}; ticket 1",
                            image),
                std::format(
                    "read: copy {0} → readback: barrier {0}: present → transfer_src; readback: home → "
                    "transfer_dst",
                    image),
                std::format("read: end: barrier {}: transfer_src → present; readback: transfer_dst → home",
                            image),
                "submit read: ticket 2",
            }));

        const auto pixels = tests::read<pixel>(*readback);
        EXPECT_TRUE(std::ranges::all_of(
            pixels, [](const pixel& value) -> bool { return value == pixel{0, 255, 0, 255}; }));
    }

    TEST_F(vulkan, a_swapchain_rebuilds_itself_and_frames_of_the_old_one_stay_presentable) {
        if (!surface()) {
            GTEST_SKIP() << "no driver here has a headless surface";
        }

        auto swapchain = vk::swapchain::make(device(), {.surface = *surface(), .extent = square});
        ASSERT_TRUE(swapchain) << swapchain.error();
        const VkSwapchainKHR first = swapchain->handle();

        auto old = swapchain->acquire(square);
        ASSERT_TRUE(old) << old.error();
        EXPECT_EQ(swapchain->handle(), first);

        auto resized = swapchain->acquire(wide);
        ASSERT_TRUE(resized) << resized.error();
        const VkSwapchainKHR second = swapchain->handle();
        EXPECT_NE(second, first);
        EXPECT_EQ(swapchain->extent().width, 128U);
        EXPECT_EQ(old->image().extent().width, 64U);
        EXPECT_EQ(resized->image().extent().width, 128U);
        EXPECT_EQ(resized->image().extent().height, 96U);

        static_cast<void>(take_trace());
        const auto untouched = std::format("unnamed swapchain image {}", old->index());
        EXPECT_TRUE(swapchain->present(std::move(*old)));
        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      std::format("submit present unnamed swapchain: first use: {0} undefined → present; "
                                  "waits for {0}; ticket 1",
                                  untouched),
                  }));
        EXPECT_TRUE(swapchain->present(std::move(*resized)));

        {
            const auto dropped = swapchain->acquire(wide);
            ASSERT_TRUE(dropped) << dropped.error();
            EXPECT_EQ(swapchain->handle(), second);
        }
        auto after_drop = swapchain->acquire(wide);
        ASSERT_TRUE(after_drop) << after_drop.error();
        const VkSwapchainKHR third = swapchain->handle();
        EXPECT_NE(third, second);
        EXPECT_TRUE(swapchain->present(std::move(*after_drop)));

        swapchain->present_mode(vk::present_mode::fifo_relaxed);
        if (auto relaxed = swapchain->acquire(wide)) {
            EXPECT_NE(swapchain->handle(), third);
            EXPECT_TRUE(swapchain->present(std::move(*relaxed)));
        } else {
            EXPECT_TRUE(has_error(relaxed, vk::error_code::invalid_argument,
                                  "the surface can't present with fifo_relaxed, it has "));
        }

        auto held = swapchain->acquire(square);
        swapchain = vk::swapchain::make(device(), {.surface = *surface(), .extent = square, .name = "next"});
        ASSERT_TRUE(swapchain) << swapchain.error();
        auto next = swapchain->acquire(square);
        ASSERT_TRUE(next) << next.error();
        EXPECT_TRUE(swapchain->present(std::move(*next)));
        held = std::unexpected(rei::error{vk::error_code::surface_unavailable});

        ASSERT_TRUE(device().wait_idle());
    }

    TEST_F(vulkan, misuse_of_a_swapchain_and_of_its_frames_surfaces_with_names) {
        if (!surface()) {
            GTEST_SKIP() << "no driver here has a headless surface";
        }

        const auto queue = device().queue();
        const auto elsewhere = vk::instance::make();
        const auto plain = vk::device::make(instance(), {.name = "plain"});
        ASSERT_TRUE(elsewhere) << elsewhere.error();
        ASSERT_TRUE(plain) << plain.error();

        EXPECT_TRUE(has_error(vk::device::make(*elsewhere, {.surface = *surface(), .name = "stray"}),
                              vk::error_code::invalid_argument,
                              "device 'stray': the surface belongs to another instance"));
        EXPECT_TRUE(has_error(vk::swapchain::make(*plain, {.surface = *surface(), .extent = square}),
                              vk::error_code::invalid_argument,
                              "an unnamed swapchain: the device was made without a surface"));
        EXPECT_TRUE(has_error(vk::swapchain::make(device(), {.surface = *surface(), .extent = {.width = 0}}),
                              vk::error_code::surface_unavailable, "there is nothing to present to at 0x1"));
        EXPECT_TRUE(
            has_error(vk::swapchain::make(device(),
                                          {
                                              .surface = *surface(),
                                              .extent = square,
                                              .format = vk::format::d32_sfloat,
                                              .name = "deep",
                                          }),
                      vk::error_code::invalid_argument,
                      "swapchain 'deep': the surface has no d32_sfloat in the sRGB color space, it has "));
        EXPECT_TRUE(has_error(vk::swapchain::make(device(),
                                                  {
                                                      .surface = *surface(),
                                                      .extent = square,
                                                      .usage = vk::image_usage::depth_stencil_attachment,
                                                  }),
                              vk::error_code::invalid_argument, "the surface's images allow the usages "));

        auto swapchain =
            vk::swapchain::make(device(), {.surface = *surface(), .extent = square, .name = "main"});
        ASSERT_TRUE(swapchain) << swapchain.error();
        EXPECT_TRUE(has_error(swapchain->acquire({.width = 64, .height = 0}),
                              vk::error_code::surface_unavailable,
                              "swapchain 'main': there is nothing to present to at 64x0"));

        auto frame = swapchain->acquire(square);
        ASSERT_TRUE(frame) << frame.error();
        const auto image = std::format("image 'main image {}'", frame->index());

        auto late = queue.record("late");
        late.render({.color = {{.target = frame->image()}}}).end();
        ASSERT_TRUE(swapchain->present(std::move(*frame)));
        EXPECT_TRUE(has_error(
            queue.submit(std::move(late)), vk::error_code::invalid_argument,
            std::format("submit of late: it uses {}, whose frame was presented or dropped", image)));
        EXPECT_TRUE(has_error(swapchain->present(std::move(*frame)), vk::error_code::invalid_argument,
                              "present to swapchain 'main': the frame was moved from"));

        const auto beside = vk::surface::headless(instance());
        ASSERT_TRUE(beside) << beside.error();
        auto other = vk::swapchain::make(device(), {.surface = *beside, .extent = square, .name = "other"});
        ASSERT_TRUE(other) << other.error();
        auto foreign = other->acquire(square);
        ASSERT_TRUE(foreign) << foreign.error();
        EXPECT_TRUE(has_error(swapchain->present(std::move(*foreign)), vk::error_code::invalid_argument,
                              "present to swapchain 'main': the frame was acquired from swapchain 'other'"));

        auto frames = std::vector<vk::frame>{};
        auto refused = false;
        while (!refused && frames.size() < 16) {
            auto another = swapchain->acquire(square);
            if (another) {
                frames.push_back(std::move(*another));
                continue;
            }

            refused = true;
            EXPECT_TRUE(has_error(another, vk::error_code::invalid_argument,
                                  std::format("acquire from swapchain 'main': {} of its frames are acquired "
                                              "and not presented",
                                              frames.size())));
        }
        EXPECT_TRUE(refused);
        EXPECT_GE(frames.size(), 1U);

        ASSERT_TRUE(device().wait_idle());
    }
} // namespace rei::gpu::tests
