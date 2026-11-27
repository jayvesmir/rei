#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "gtest/gtest.h"

#include "rei/gpu/tests/shaders/doubling.hpp"
#include "rei/gpu/tests/support.hpp"
#include "rei/gpu/vk.hpp"
#include "rei/gpu/vk/raw.hpp"

namespace rei::gpu::tests {
    namespace {
        struct color_target {
            VkImage image;
            VkImageView view;
        };

        struct compute_pipeline {
            VkPipeline handle;
            VkPipelineLayout layout;
        };

        class raw_objects {
          private:
            const vk::device& m_device;
            std::vector<std::function<void()>> m_destroyers;

            auto _allocate(const VkMemoryRequirements& requirements) -> VkDeviceMemory {
                const auto info = VkMemoryAllocateInfo{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize = requirements.size,
                    .memoryTypeIndex =
                        static_cast<std::uint32_t>(std::countr_zero(requirements.memoryTypeBits)),
                };

                VkDeviceMemory memory{};
                EXPECT_EQ(m_device.raw().vkAllocateMemory(m_device.handle(), &info, nullptr, &memory),
                          VK_SUCCESS);

                return memory;
            }

          public:
            explicit raw_objects(const vk::device& device) noexcept : m_device(device) {}
            raw_objects(const raw_objects& other) = delete;
            auto operator=(const raw_objects& other) -> raw_objects& = delete;

            ~raw_objects() {
                for (const auto& destroy : m_destroyers | std::views::reverse) {
                    destroy();
                }
            }

            auto storage_buffer(VkDeviceSize size) -> VkBuffer {
                const auto& raw = m_device.raw();
                const VkDevice device = m_device.handle();
                const auto info = VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = size,
                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                };

                VkBuffer buffer{};
                EXPECT_EQ(raw.vkCreateBuffer(device, &info, nullptr, &buffer), VK_SUCCESS);

                auto requirements = VkMemoryRequirements{};
                raw.vkGetBufferMemoryRequirements(device, buffer, &requirements);
                const VkDeviceMemory memory = _allocate(requirements);
                EXPECT_EQ(raw.vkBindBufferMemory(device, buffer, memory, 0), VK_SUCCESS);

                m_destroyers.emplace_back([&raw, device, buffer, memory] -> void {
                    raw.vkDestroyBuffer(device, buffer, nullptr);
                    raw.vkFreeMemory(device, memory, nullptr);
                });

                return buffer;
            }

            auto color_target() -> tests::color_target {
                const auto& raw = m_device.raw();
                const VkDevice device = m_device.handle();
                const auto info = VkImageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent = {.width = 64, .height = 64, .depth = 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                };

                VkImage image{};
                EXPECT_EQ(raw.vkCreateImage(device, &info, nullptr, &image), VK_SUCCESS);

                auto requirements = VkMemoryRequirements{};
                raw.vkGetImageMemoryRequirements(device, image, &requirements);
                const VkDeviceMemory memory = _allocate(requirements);
                EXPECT_EQ(raw.vkBindImageMemory(device, image, memory, 0), VK_SUCCESS);

                const auto view_info = VkImageViewCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = image,
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = info.format,
                    .subresourceRange =
                        {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .levelCount = 1,
                            .layerCount = 1,
                        },
                };
                VkImageView view{};
                EXPECT_EQ(raw.vkCreateImageView(device, &view_info, nullptr, &view), VK_SUCCESS);

                m_destroyers.emplace_back([&raw, device, image, view, memory] -> void {
                    raw.vkDestroyImageView(device, view, nullptr);
                    raw.vkDestroyImage(device, image, nullptr);
                    raw.vkFreeMemory(device, memory, nullptr);
                });

                return {.image = image, .view = view};
            }

            auto compute_pipeline(std::span<const std::uint32_t> spirv) -> tests::compute_pipeline {
                const auto& raw = m_device.raw();
                const VkDevice device = m_device.handle();

                const auto binding = VkDescriptorSetLayoutBinding{
                    .binding = 0,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                };
                const auto set_info = VkDescriptorSetLayoutCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                    .bindingCount = 1,
                    .pBindings = &binding,
                };
                VkDescriptorSetLayout set_layout{};
                EXPECT_EQ(raw.vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout),
                          VK_SUCCESS);

                const auto layout_info = VkPipelineLayoutCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                    .setLayoutCount = 1,
                    .pSetLayouts = &set_layout,
                };
                VkPipelineLayout layout{};
                EXPECT_EQ(raw.vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), VK_SUCCESS);

                const auto shader = VkShaderModuleCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .codeSize = spirv.size_bytes(),
                    .pCode = spirv.data(),
                };
                const auto info = VkComputePipelineCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage =
                        {
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            .pNext = &shader,
                            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                            .pName = "main",
                        },
                    .layout = layout,
                };
                VkPipeline pipeline{};
                EXPECT_EQ(raw.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                          VK_SUCCESS);

                m_destroyers.emplace_back([&raw, device, pipeline, layout, set_layout] -> void {
                    raw.vkDestroyPipeline(device, pipeline, nullptr);
                    raw.vkDestroyPipelineLayout(device, layout, nullptr);
                    raw.vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
                });

                return {.handle = pipeline, .layout = layout};
            }

            template <typename RecordType> auto run(const RecordType& record) -> void {
                const auto& raw = m_device.raw();
                const VkDevice device = m_device.handle();
                const auto queue = m_device.queue();

                const auto pool_info = VkCommandPoolCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                    .queueFamilyIndex = queue.family(),
                };
                VkCommandPool pool{};
                ASSERT_EQ(raw.vkCreateCommandPool(device, &pool_info, nullptr, &pool), VK_SUCCESS);
                m_destroyers.emplace_back(
                    [&raw, device, pool] -> void { raw.vkDestroyCommandPool(device, pool, nullptr); });

                const auto allocation = VkCommandBufferAllocateInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                    .commandPool = pool,
                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                    .commandBufferCount = 1,
                };
                VkCommandBuffer commands{};
                ASSERT_EQ(raw.vkAllocateCommandBuffers(device, &allocation, &commands), VK_SUCCESS);

                const auto begin =
                    VkCommandBufferBeginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                ASSERT_EQ(raw.vkBeginCommandBuffer(commands, &begin), VK_SUCCESS);
                record(commands);
                ASSERT_EQ(raw.vkEndCommandBuffer(commands), VK_SUCCESS);

                const auto submitted = VkCommandBufferSubmitInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = commands,
                };
                const auto submit = VkSubmitInfo2{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .commandBufferInfoCount = 1,
                    .pCommandBufferInfos = &submitted,
                };
                ASSERT_EQ(raw.vkQueueSubmit2(queue.handle(), 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
                ASSERT_EQ(raw.vkQueueWaitIdle(queue.handle()), VK_SUCCESS);
            }
        };

        auto only_hazards(const std::vector<std::string>& messages) -> testing::AssertionResult {
            if (messages.empty()) {
                return testing::AssertionFailure() << "the validation layer reported nothing";
            }

            const auto other = std::ranges::find_if_not(messages, [](const std::string& message) -> bool {
                return message.starts_with("SYNC-HAZARD-");
            });
            if (other != messages.end()) {
                return testing::AssertionFailure() << "unexpected message " << *other;
            }

            return testing::AssertionSuccess();
        }
    } // namespace

    TEST_F(vulkan,
           the_validation_layer_reports_a_render_that_loads_what_the_last_one_stored_without_a_barrier) {
        auto objects = raw_objects{device()};
        const auto target = objects.color_target();

        objects.run([&](VkCommandBuffer commands) -> void {
            const auto first_use = VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .dstAccessMask =
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target.image,
                .subresourceRange =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1,
                        .layerCount = 1,
                    },
            };
            const auto dependency = VkDependencyInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &first_use,
            };
            device().raw().vkCmdPipelineBarrier2(commands, &dependency);

            for (const auto load : {VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_LOAD}) {
                const auto color = VkRenderingAttachmentInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = target.view,
                    .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                    .loadOp = load,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                };
                const auto info = VkRenderingInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea = {.extent = {.width = 64, .height = 64}},
                    .layerCount = 1,
                    .colorAttachmentCount = 1,
                    .pColorAttachments = &color,
                };

                device().raw().vkCmdBeginRendering(commands, &info);
                device().raw().vkCmdEndRendering(commands);
            }
        });

        EXPECT_TRUE(only_hazards(take_messages()));
    }

    TEST_F(vulkan,
           the_validation_layer_reports_a_dispatch_that_writes_what_the_last_one_wrote_without_a_barrier) {
        auto objects = raw_objects{device()};
        const VkBuffer numbers = objects.storage_buffer(1024 * sizeof(float));
        const auto pipeline = objects.compute_pipeline(shaders::doubling::spirv);

        objects.run([&](VkCommandBuffer commands) -> void {
            const auto& raw = device().raw();
            const auto buffer = VkDescriptorBufferInfo{.buffer = numbers, .range = VK_WHOLE_SIZE};
            const auto write = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffer,
            };

            raw.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
            raw.vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1,
                                       &write);
            raw.vkCmdDispatch(commands, 1024 / 64, 1, 1);
            raw.vkCmdDispatch(commands, 1024 / 64, 1, 1);
        });

        EXPECT_TRUE(only_hazards(take_messages()));
    }
} // namespace rei::gpu::tests
