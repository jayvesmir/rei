#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/gpu/tests/support.hpp"
#include "rei/gpu/vk.hpp"
#include "rei/gpu/vk/raw.hpp"

namespace rei::gpu::tests {
    namespace {
        using namespace std::chrono_literals;

        class gate {
          private:
            const vk::device& m_device;
            VkSemaphore m_semaphore{};
            VkCommandPool m_pool{};

          public:
            explicit gate(const vk::device& device) : m_device(device) {
                const auto& raw = device.raw();
                const auto queue = device.queue();

                const auto type = VkSemaphoreTypeCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                };
                const auto semaphore = VkSemaphoreCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                    .pNext = &type,
                };
                EXPECT_EQ(raw.vkCreateSemaphore(device.handle(), &semaphore, nullptr, &m_semaphore),
                          VK_SUCCESS);

                const auto pool = VkCommandPoolCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                    .queueFamilyIndex = queue.family(),
                };
                EXPECT_EQ(raw.vkCreateCommandPool(device.handle(), &pool, nullptr, &m_pool), VK_SUCCESS);

                const auto allocation = VkCommandBufferAllocateInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool = m_pool,
                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                    .commandBufferCount = 1,
                };
                VkCommandBuffer commands{};
                EXPECT_EQ(raw.vkAllocateCommandBuffers(device.handle(), &allocation, &commands), VK_SUCCESS);

                const auto begin =
                    VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                const auto barrier = VkMemoryBarrier2{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                };
                const auto dependency = VkDependencyInfo{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1,
                    .pMemoryBarriers = &barrier,
                };
                EXPECT_EQ(raw.vkBeginCommandBuffer(commands, &begin), VK_SUCCESS);
                raw.vkCmdPipelineBarrier2(commands, &dependency);
                EXPECT_EQ(raw.vkEndCommandBuffer(commands), VK_SUCCESS);

                const auto wait = VkSemaphoreSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = m_semaphore,
                    .value = 1,
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                };
                const auto submitted = VkCommandBufferSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = commands,
                };
                const auto submit = VkSubmitInfo2{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .waitSemaphoreInfoCount = 1,
                    .pWaitSemaphoreInfos = &wait,
                    .commandBufferInfoCount = 1,
                    .pCommandBufferInfos = &submitted,
                };
                EXPECT_EQ(raw.vkQueueSubmit2(queue.handle(), 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
            }

            gate(const gate& other) = delete;
            auto operator=(const gate& other) -> gate& = delete;

            ~gate() {
                m_device.raw().vkDestroyCommandPool(m_device.handle(), m_pool, nullptr);
                m_device.raw().vkDestroySemaphore(m_device.handle(), m_semaphore, nullptr);
            }

            auto open() -> void {
                const auto signal = VkSemaphoreSignalInfo{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                    .semaphore = m_semaphore,
                    .value = 1,
                };
                EXPECT_EQ(m_device.raw().vkSignalSemaphore(m_device.handle(), &signal), VK_SUCCESS);
            }
        };
    } // namespace

    TEST_F(vulkan, a_buffer_is_uploaded_copied_and_read_back_with_the_barriers_rei_owes) {
        auto values = std::array<std::uint32_t, 256>{};
        for (auto i = 0uz; i < values.size(); i++) {
            values[i] = static_cast<std::uint32_t>(i);
        }

        auto source = vk::buffer::make(
            device(), {
                          .size = sizeof(values),
                          .usage = vk::buffer_usage::transfer_src | vk::buffer_usage::transfer_dst,
                          .name = "source",
                      });
        auto target = vk::buffer::make(device(), {
                                                     .size = sizeof(values),
                                                     .usage = vk::buffer_usage::transfer_dst,
                                                     .host = vk::host_access::read_write,
                                                     .name = "target",
                                                 });
        ASSERT_TRUE(source) << source.error();
        ASSERT_TRUE(target) << target.error();

        auto commands = device().queue().record("transfer");
        commands.upload(*source, std::as_bytes(std::span{values}));
        commands.copy(*source, *target);

        const auto ticket = device().queue().submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        auto read = std::array<std::uint32_t, 256>{};
        ASSERT_TRUE(target->read(std::as_writable_bytes(std::span{read})));
        EXPECT_EQ(read, values);

        EXPECT_EQ(take_trace(), (std::vector<std::string>{
                                    "transfer: upload source: staging buffer, 1024 bytes",
                                    "transfer: upload source: barrier source: home → transfer_dst",
                                    "transfer: upload source: copy staging → source",
                                    "transfer: copy source → target: barrier source: transfer_dst → home; "
                                    "target: home → transfer_dst",
                                    "transfer: end: barrier target: transfer_dst → home",
                                    "submit transfer: ticket 1",
                                    "collect: freed staging",
                                }));
    }

    TEST_F(vulkan, an_image_level_is_uploaded_and_read_back_through_folded_barriers) {
        auto pixels = std::vector<std::uint8_t>(4 * 4 * 4);
        for (auto i = 0uz; i < pixels.size(); i++) {
            pixels[i] = static_cast<std::uint8_t>(i);
        }

        auto image =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 8, .height = 8},
                                          .usage = vk::image_usage::sampled | vk::image_usage::transfer_src |
                                                   vk::image_usage::transfer_dst,
                                          .mip_levels = 2,
                                          .name = "pixels",
                                      });
        auto readback = vk::buffer::make(device(), {
                                                       .size = pixels.size(),
                                                       .usage = vk::buffer_usage::transfer_dst,
                                                       .host = vk::host_access::read_write,
                                                       .name = "readback",
                                                   });
        ASSERT_TRUE(image) << image.error();
        ASSERT_TRUE(readback) << readback.error();
        EXPECT_EQ(image->home(), vk::access::shader_read);

        auto commands = device().queue().record("image");
        commands.upload(*image, std::as_bytes(std::span{pixels}), {.mip = 1});
        commands.copy(*image, *readback, {.mip = 1});

        const auto ticket = device().queue().submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        auto read = std::vector<std::uint8_t>(pixels.size());
        ASSERT_TRUE(readback->read(std::as_writable_bytes(std::span{read})));
        EXPECT_EQ(read, pixels);

        EXPECT_EQ(
            take_trace(),
            (std::vector<std::string>{
                "image: upload pixels: staging buffer, 64 bytes",
                "image: upload pixels: barrier pixels mip 1: shader_read → transfer_dst, discard",
                "image: upload pixels: copy staging → pixels mip 1",
                "image: copy pixels → readback: barrier pixels mip 1: transfer_dst → transfer_src (folded); "
                "readback: "
                "home → transfer_dst",
                "image: end: barrier pixels mip 1: transfer_src → shader_read; readback: transfer_dst → home",
                "submit image: first use: pixels undefined → shader_read; ticket 1",
                "collect: freed staging",
            }));
    }

    TEST_F(vulkan, a_buffer_destroyed_right_after_submit_is_freed_only_once_its_work_is_done) {
        const auto queue = device().queue();
        auto blocked = gate{device()};

        auto ticket = std::optional<vk::ticket>{};
        {
            auto doomed = vk::buffer::make(device(), {
                                                         .size = 1024,
                                                         .usage = vk::buffer_usage::transfer_dst,
                                                         .name = "doomed",
                                                     });
            ASSERT_TRUE(doomed) << doomed.error();

            const auto sevens = std::vector<std::byte>(1024, std::byte{7});
            auto commands = queue.record("doomed");
            commands.upload(*doomed, sevens);

            const auto submitted = queue.submit(std::move(commands));
            ASSERT_TRUE(submitted) << submitted.error();
            ticket = *submitted;
        }

        device().collect();
        EXPECT_FALSE(device().done(*ticket));
        EXPECT_TRUE(
            has_error(device().wait(*ticket, 1ms), vk::error_code::timed_out, "ticket 1 is not done"));
        EXPECT_FALSE(std::ranges::any_of(
            take_trace(), [](const std::string& line) -> bool { return line.starts_with("collect"); }));

        blocked.open();
        ASSERT_TRUE(device().wait(*ticket));
        EXPECT_TRUE(device().done(*ticket));
        EXPECT_EQ(take_trace(), std::vector<std::string>{"collect: freed staging, doomed"});

        EXPECT_TRUE(has_error(device().wait({.value = 2}), vk::error_code::invalid_argument,
                              "ticket 2, which was never issued"));
        ASSERT_TRUE(device().wait_idle());
    }

    TEST_F(vulkan, an_image_destroyed_before_the_submit_of_its_first_use_is_still_moved_home_first) {
        const auto queue = device().queue();
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 16 * 16 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        ASSERT_TRUE(readback) << readback.error();

        auto commands = queue.record("short-lived");
        {
            const auto scratch = vk::image::make(
                device(), {
                              .format = vk::format::r8g8b8a8_unorm,
                              .extent = {.width = 16, .height = 16},
                              .usage = vk::image_usage::sampled | vk::image_usage::transfer_src,
                              .name = "scratch",
                          });
            ASSERT_TRUE(scratch) << scratch.error();
            commands.copy(*scratch, *readback);
        }

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "short-lived: copy scratch → readback: barrier scratch: shader_read → transfer_src; "
                      "readback: home → transfer_dst",
                      "short-lived: end: barrier scratch: transfer_src → shader_read; readback: transfer_dst → "
                      "home",
                      "submit short-lived: first use: scratch undefined → shader_read; ticket 1",
                      "collect: freed scratch",
                  }));
    }

    TEST_F(vulkan, a_recording_failure_surfaces_at_submit_with_names_and_nothing_recorded_runs) {
        const auto queue = device().queue();
        const auto zeros = std::vector<std::byte>(16, std::byte{0});
        const auto ones = std::vector<std::byte>(16, std::byte{1});

        auto target = vk::buffer::make(device(), {
                                                     .size = 16,
                                                     .usage = vk::buffer_usage::transfer_dst,
                                                     .host = vk::host_access::read_write,
                                                     .name = "target",
                                                 });
        auto sealed = vk::buffer::make(
            device(), {.size = 16, .usage = vk::buffer_usage::storage_buffer, .name = "sealed"});
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(sealed) << sealed.error();
        ASSERT_TRUE(target->write(zeros));

        auto failing = queue.record("failing");
        failing.upload(*target, ones);
        failing.upload(*sealed, ones);
        failing.upload(*target, ones);
        EXPECT_TRUE(has_error(queue.submit(std::move(failing)), vk::error_code::invalid_argument,
                              "upload into buffer 'sealed': its usage lacks transfer_dst"));

        ASSERT_TRUE(device().wait_idle());
        auto read = std::vector<std::byte>(16, std::byte{9});
        ASSERT_TRUE(target->read(read));
        EXPECT_EQ(read, zeros);
        EXPECT_EQ(take_trace(), (std::vector<std::string>{
                                    "failing: upload target: staging buffer, 16 bytes",
                                    "failing: upload target: barrier target: home → transfer_dst",
                                    "failing: upload target: copy staging → target",
                                    "collect: freed staging",
                                }));

        auto texture =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 4, .height = 4},
                                          .usage = vk::image_usage::sampled | vk::image_usage::transfer_dst,
                                          .name = "texture",
                                      });
        ASSERT_TRUE(texture) << texture.error();

        auto window = std::optional<vk::image_view>{};
        {
            auto gone = vk::image::make(device(), {
                                                      .format = vk::format::r8g8b8a8_unorm,
                                                      .extent = {.width = 4, .height = 4},
                                                      .usage = vk::image_usage::sampled,
                                                      .name = "gone",
                                                  });
            ASSERT_TRUE(gone) << gone.error();

            auto made = vk::image_view::make(*gone, {.name = "window"});
            ASSERT_TRUE(made) << made.error();
            window = std::move(*made);
        }

        const auto elsewhere_device = vk::device::make(instance());
        ASSERT_TRUE(elsewhere_device) << elsewhere_device.error();
        auto elsewhere = vk::buffer::make(*elsewhere_device, {
                                                                 .size = 16,
                                                                 .usage = vk::buffer_usage::transfer_dst,
                                                                 .name = "elsewhere",
                                                             });
        ASSERT_TRUE(elsewhere) << elsewhere.error();

        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void { commands.upload(*target, std::vector<std::byte>(17)); },
            "upload into buffer 'target': 17 bytes at offset 0 go past its end at 16"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void { commands.upload(*texture, std::vector<std::byte>(3)); },
            "upload into image 'texture': 3 bytes given, and mip 0 of r8g8b8a8_unorm 4x4x1 holds 64"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void { commands.copy(*texture, *target); },
            "copy from image 'texture' into buffer 'target': the source needs the transfer_src usage"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                commands.barrier(
                    {{.target = *window, .from = vk::access::shader_read, .to = vk::access::transfer_dst}});
            },
            "barrier on image view 'window': its image 'gone' was destroyed"));
        EXPECT_TRUE(misuse([&](vk::commands& commands) -> void { commands.upload(*elsewhere, ones); },
                           "buffer 'elsewhere' belongs to another device"));

        EXPECT_TRUE(has_error(queue.submit(queue.record("late"), {vk::ticket{.value = 99}}),
                              vk::error_code::invalid_argument, "submit of late waits for ticket 99"));
    }

    TEST_F(vulkan, making_what_vulkan_would_reject_fails_and_names_the_object) {
        EXPECT_TRUE(
            has_error(vk::buffer::make(
                          device(), {.size = 0, .usage = vk::buffer_usage::storage_buffer, .name = "empty"}),
                      vk::error_code::invalid_argument, "buffer 'empty': size is 0"));
        EXPECT_TRUE(has_error(vk::image::make(device(),
                                              {
                                                  .format = vk::format::r8g8b8a8_unorm,
                                                  .extent = {.width = 4, .height = 4},
                                                  .usage = vk::image_usage::transfer_dst,
                                                  .home = vk::access::shader_read,
                                                  .name = "misplaced",
                                              }),
                              vk::error_code::invalid_argument,
                              "image 'misplaced': home shader_read needs the sampled usage"));
        EXPECT_TRUE(has_error(vk::image::make(device(),
                                              {
                                                  .format = vk::format::r8g8b8a8_unorm,
                                                  .extent = {.width = 4, .height = 4},
                                                  .usage = vk::image_usage::sampled,
                                                  .mip_levels = 4,
                                                  .name = "deep",
                                              }),
                              vk::error_code::invalid_argument,
                              "image 'deep': 4 mip levels, and 4x4x1 has room for 1 to 3"));
        EXPECT_TRUE(has_error(vk::sampler::make(device(), {.anisotropy = 4.0f, .name = "sharp"}),
                              vk::error_code::missing_feature,
                              "sampler 'sharp': anisotropy needs the feature sampler_anisotropy"));

        auto texture = vk::image::make(device(), {
                                                     .format = vk::format::r8g8b8a8_unorm,
                                                     .extent = {.width = 4, .height = 4},
                                                     .usage = vk::image_usage::sampled,
                                                     .name = "texture",
                                                 });
        ASSERT_TRUE(texture) << texture.error();
        EXPECT_TRUE(has_error(vk::image_view::make(*texture, {.first_mip = 1, .name = "beyond"}),
                              vk::error_code::invalid_argument, "image view 'beyond': 0 mip levels from 1"));

        auto staging = vk::buffer::make(device(), {
                                                      .size = 16,
                                                      .usage = vk::buffer_usage::transfer_src,
                                                      .host = vk::host_access::write,
                                                      .name = "staging",
                                                  });
        ASSERT_TRUE(staging) << staging.error();
        EXPECT_TRUE(has_error(staging->write(std::vector<std::byte>(20)), vk::error_code::invalid_argument,
                              "write into buffer 'staging': 20 bytes at offset 0 go past its end at 16"));

        auto read = std::vector<std::byte>(4);
        EXPECT_TRUE(has_error(staging->read(read), vk::error_code::invalid_argument,
                              "read from buffer 'staging': reading needs host_access::read_write"));
    }

    TEST_F(vulkan, commands_recorded_on_two_threads_can_be_submitted_in_reverse_order) {
        const auto queue = device().queue();

        auto first = vk::buffer::make(device(), {
                                                    .size = 1024,
                                                    .usage = vk::buffer_usage::transfer_dst,
                                                    .host = vk::host_access::read_write,
                                                    .name = "first",
                                                });
        auto second = vk::buffer::make(device(), {
                                                     .size = 1024,
                                                     .usage = vk::buffer_usage::transfer_dst,
                                                     .host = vk::host_access::read_write,
                                                     .name = "second",
                                                 });
        ASSERT_TRUE(first) << first.error();
        ASSERT_TRUE(second) << second.error();

        const auto ones = std::vector<std::byte>(1024, std::byte{1});
        const auto twos = std::vector<std::byte>(1024, std::byte{2});
        auto recorded = std::array<std::optional<vk::commands>, 2>{};
        {
            auto one = std::thread{[&] -> void {
                auto commands = queue.record("first");
                commands.upload(*first, ones);
                recorded[0] = std::move(commands);
            }};
            auto two = std::thread{[&] -> void {
                auto commands = queue.record("second");
                commands.upload(*second, twos);
                recorded[1] = std::move(commands);
            }};

            one.join();
            two.join();
        }

        const auto later = queue.submit(std::move(*recorded[1]));
        const auto earlier = queue.submit(std::move(*recorded[0]));
        ASSERT_TRUE(later) << later.error();
        ASSERT_TRUE(earlier) << earlier.error();
        ASSERT_TRUE(device().wait(*earlier));

        auto read = std::vector<std::byte>(1024);
        ASSERT_TRUE(first->read(read));
        EXPECT_EQ(read, ones);
        ASSERT_TRUE(second->read(read));
        EXPECT_EQ(read, twos);
    }
} // namespace rei::gpu::tests
