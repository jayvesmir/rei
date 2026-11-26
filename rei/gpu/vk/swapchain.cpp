#include <algorithm>
#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/detail/formats.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/swapchain.hpp"

namespace rei::gpu::vk {
    namespace {
        auto name_of(VkPresentModeKHR mode) -> std::string {
            switch (mode) {
            case VK_PRESENT_MODE_IMMEDIATE_KHR:
                return "immediate";
            case VK_PRESENT_MODE_MAILBOX_KHR:
                return "mailbox";
            case VK_PRESENT_MODE_FIFO_KHR:
                return "fifo";
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
                return "fifo_relaxed";
            default:
                return std::format("mode {}", std::to_underlying(mode));
            }
        }
    } // namespace

    swapchain::generation::generation(std::shared_ptr<vk::device::state> device, vk::surface surface) noexcept
        : device(std::move(device)), surface(std::move(surface)) {}

    swapchain::generation::~generation() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        auto semaphores = std::move(spare);
        for (const auto& slot : slots) {
            if (slot.acquired != VK_NULL_HANDLE) {
                semaphores.push_back(slot.acquired);
            }
            semaphores.append_range(slot.rendered);
        }
        slots.clear();

        device->retire({
            .label = detail::label(name, "swapchain"),
            .swapchain = handle,
            .semaphores = std::move(semaphores),
            .surface = surface,
            .ticket = presented,
        });
    }

    frame::state::~state() {
        if (presented) {
            return;
        }

        const auto lock = std::scoped_lock{generation->mutex};
        generation->slots[index].held = false;
        generation->held--;
        generation->stale = true;
    }

    auto swapchain::state::build(vk::extent wanted) -> std::expected<void, rei::error> {
        auto& context = *device;
        const auto& instance = context.instance.raw();
        const auto what = detail::quoted(name, "swapchain");
        const auto present_mode = static_cast<VkPresentModeKHR>(mode);

        auto capabilities = VkSurfaceCapabilitiesKHR{};
        if (const auto result = instance.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                context.physical.handle, surface.handle(), &capabilities);
            result != VK_SUCCESS) {
            return std::unexpected(
                detail::vulkan_error(result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", what));
        }

        const auto modes = detail::enumerate<VkPresentModeKHR>(
            "vkGetPhysicalDeviceSurfacePresentModesKHR",
            [&](std::uint32_t* count, VkPresentModeKHR* items) -> VkResult {
                return instance.vkGetPhysicalDeviceSurfacePresentModesKHR(context.physical.handle,
                                                                          surface.handle(), count, items);
            });
        if (!modes) {
            return std::unexpected(modes.error());
        }
        if (!std::ranges::contains(*modes, present_mode)) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the surface can't present with {}, it has {}", what, name_of(present_mode),
                            *modes | std::views::transform(name_of) |
                                std::views::join_with(std::string_view{", "}) |
                                std::ranges::to<std::string>()),
            });
        }

        const auto& smallest = capabilities.minImageExtent;
        const auto& largest = capabilities.maxImageExtent;
        if (wanted.width == 0 || wanted.height == 0 || largest.width == 0 || largest.height == 0) {
            return std::unexpected(rei::error{
                vk::error_code::surface_unavailable,
                std::format("{}: there is nothing to present to at {}x{}, the window is probably minimized",
                            what, std::min(wanted.width, largest.width),
                            std::min(wanted.height, largest.height)),
            });
        }
        if ((capabilities.supportedUsageFlags & std::to_underlying(usage)) != std::to_underlying(usage)) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the surface's images allow the usages {:#x}, and {:#x} was asked for", what,
                            capabilities.supportedUsageFlags, std::to_underlying(usage)),
            });
        }

        const auto size = vk::extent{
            .width = std::clamp(wanted.width, smallest.width, largest.width),
            .height = std::clamp(wanted.height, smallest.height, largest.height),
        };
        const auto unbounded = capabilities.maxImageCount == 0;
        const auto opaque = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0;

        auto& shared = *surface.m_state;
        auto created = std::make_shared<swapchain::generation>(device, surface);
        {
            const auto lock = std::scoped_lock{shared.mutex};
            const VkSwapchainKHR replaced = shared.presenter == context.handle ? shared.live : VK_NULL_HANDLE;
            const auto info = VkSwapchainCreateInfoKHR{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                .surface = surface.handle(),
                .minImageCount = std::min(capabilities.minImageCount + 1,
                                          unbounded ? std::numeric_limits<std::uint32_t>::max()
                                                    : capabilities.maxImageCount),
                .imageFormat = format.format,
                .imageColorSpace = format.colorSpace,
                .imageExtent = {.width = size.width, .height = size.height},
                .imageArrayLayers = 1,
                .imageUsage = static_cast<VkImageUsageFlags>(usage),
                .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .preTransform = capabilities.currentTransform,
                .compositeAlpha = opaque ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                         : static_cast<VkCompositeAlphaFlagBitsKHR>(
                                               std::bit_floor(capabilities.supportedCompositeAlpha)),
                .presentMode = present_mode,
                .clipped = VK_TRUE,
                .oldSwapchain = replaced,
            };

            const auto result =
                context.table.vkCreateSwapchainKHR(context.handle, &info, nullptr, &created->handle);
            if (result != VK_SUCCESS && replaced != VK_NULL_HANDLE) {
                shared.live = VK_NULL_HANDLE;
            }
            if (result != VK_SUCCESS) {
                return std::unexpected(detail::vulkan_error(result, "vkCreateSwapchainKHR", what));
            }

            shared.presenter = context.handle;
            shared.live = created->handle;
        }

        created->extent = size;
        created->mode = mode;
        created->min_images = capabilities.minImageCount;
        created->name = name;
        context.name(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<std::uint64_t>(created->handle), name);

        const auto images = detail::enumerate<VkImage>(
            "vkGetSwapchainImagesKHR", [&](std::uint32_t* count, VkImage* items) -> VkResult {
                return context.table.vkGetSwapchainImagesKHR(context.handle, created->handle, count, items);
            });
        if (!images) {
            return std::unexpected(images.error());
        }

        for (auto i = 0uz; i < images->size(); i++) {
            auto image = std::make_shared<image::state>();
            image->device = device;
            image->handle = (*images)[i];
            image->format = static_cast<vk::format>(format.format);
            image->extent = size;
            image->usage = usage;
            image->home = vk::access::present;
            image->name = std::format("{} image {}", detail::label(name, "swapchain"), i);
            image->swapchain = created;
            image->swapchain_index = static_cast<std::uint32_t>(i);

            context.name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(image->handle), image->name);

            auto view = image::_view(image, image->range(), image->name, true);
            if (!view) {
                return std::unexpected(view.error());
            }

            created->slots.push_back({.image = vk::image{std::move(image), std::move(*view)}});
        }

        context.trace("swapchain {}: {} images of {} at {}x{}, {}", detail::label(name, "swapchain"),
                      created->slots.size(), detail::info_of(static_cast<vk::format>(format.format)).name,
                      size.width, size.height, name_of(present_mode));

        current = std::move(created);
        requested = wanted;
        extent = size;

        return {};
    }

    frame::frame(std::unique_ptr<frame::state> state) noexcept : m_state(std::move(state)) {}

    frame::frame(frame&& other) noexcept = default;

    auto frame::operator=(frame&& other) noexcept -> frame& = default;

    frame::~frame() = default;

    auto frame::image() const noexcept -> const vk::image& {
        return m_state->generation->slots[m_state->index].image;
    }

    auto frame::index() const noexcept -> std::uint32_t {
        return m_state->index;
    }

    swapchain::swapchain(std::unique_ptr<swapchain::state> state) noexcept : m_state(std::move(state)) {}

    auto swapchain::make(const vk::device& device, const vk::swapchain_options& options)
        -> std::expected<vk::swapchain, rei::error> {
        const auto& context = *device.m_state;
        const auto& instance = context.instance.raw();
        const auto what = detail::quoted(options.name, "swapchain");

        if (!context.presenting) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the device was made without a surface, so it can't present", what),
            });
        }
        if (options.surface.m_state->instance.handle() != context.instance.handle()) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: the surface belongs to another instance", what),
            });
        }

        auto presentable = VkBool32{VK_FALSE};
        if (const auto result = instance.vkGetPhysicalDeviceSurfaceSupportKHR(
                context.physical.handle, context.queue.family, options.surface.handle(), &presentable);
            result != VK_SUCCESS) {
            return std::unexpected(
                detail::vulkan_error(result, "vkGetPhysicalDeviceSurfaceSupportKHR", what));
        }
        if (presentable != VK_TRUE) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("{}: '{}' can't present to this surface", what, context.physical.name),
            });
        }

        const auto formats = detail::enumerate<VkSurfaceFormatKHR>(
            "vkGetPhysicalDeviceSurfaceFormatsKHR",
            [&](std::uint32_t* count, VkSurfaceFormatKHR* items) -> VkResult {
                return instance.vkGetPhysicalDeviceSurfaceFormatsKHR(context.physical.handle,
                                                                     options.surface.handle(), count, items);
            });
        if (!formats) {
            return std::unexpected(formats.error());
        }

        const auto preferred = options.format
                                   ? std::vector{*options.format}
                                   : std::vector{vk::format::b8g8r8a8_srgb, vk::format::r8g8b8a8_srgb};
        auto chosen = formats->end();
        for (const auto format : preferred) {
            if (chosen == formats->end()) {
                chosen = std::ranges::find_if(*formats, [&](const VkSurfaceFormatKHR& offered) -> bool {
                    return offered.format == static_cast<VkFormat>(format) &&
                           offered.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
            }
        }
        if (chosen == formats->end() && options.format) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format(
                    "{}: the surface has no {} in the sRGB color space, it has {}", what,
                    detail::info_of(*options.format).name,
                    *formats | std::views::filter([](const VkSurfaceFormatKHR& offered) -> bool {
                        return offered.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                    }) | std::views::transform([](const VkSurfaceFormatKHR& offered) -> std::string_view {
                        return detail::info_of(static_cast<vk::format>(offered.format)).name;
                    }) | std::views::join_with(std::string_view{", "}) |
                        std::ranges::to<std::string>()),
            });
        }

        auto state = std::make_unique<swapchain::state>(swapchain::state{
            .device = device.m_state,
            .surface = options.surface,
            .format = chosen == formats->end() ? formats->front() : *chosen,
            .usage = options.usage,
            .mode = options.present_mode,
            .name = options.name,
        });

        if (const auto built = state->build(options.extent); !built) {
            return std::unexpected(built.error());
        }

        return vk::swapchain{std::move(state)};
    }

    swapchain::swapchain(swapchain&& other) noexcept = default;

    auto swapchain::operator=(swapchain&& other) noexcept -> swapchain& = default;

    swapchain::~swapchain() = default;

    auto swapchain::acquire(vk::extent extent) -> std::expected<vk::frame, rei::error> {
        auto& self = *m_state;
        auto& context = *self.device;
        const auto what = detail::quoted(self.name, "swapchain");

        for (auto attempt = 0U; attempt < 3U; attempt++) {
            if (!self.current || self.current->stale || self.current->mode != self.mode ||
                extent.width != self.requested.width || extent.height != self.requested.height) {
                if (const auto built = self.build(extent); !built) {
                    return std::unexpected(built.error());
                }
            }

            auto& generation = *self.current;
            auto lock = std::unique_lock{generation.mutex};

            if (generation.held > generation.slots.size() - generation.min_images) {
                return std::unexpected(rei::error{
                    vk::error_code::invalid_argument,
                    std::format("acquire from {}: {} of its frames are acquired and not presented, and its "
                                "{} images leave room for {}",
                                what, generation.held, generation.slots.size(),
                                generation.slots.size() - generation.min_images + 1),
                });
            }

            VkSemaphore semaphore{};
            if (generation.spare.empty()) {
                const auto info = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                if (const auto result =
                        context.table.vkCreateSemaphore(context.handle, &info, nullptr, &semaphore);
                    result != VK_SUCCESS) {
                    return std::unexpected(detail::vulkan_error(result, "vkCreateSemaphore", what));
                }
            } else {
                semaphore = generation.spare.back();
                generation.spare.pop_back();
            }
            lock.unlock();

            auto index = std::uint32_t{};
            const auto result = context.table.vkAcquireNextImageKHR(context.handle, generation.handle,
                                                                    std::numeric_limits<std::uint64_t>::max(),
                                                                    semaphore, VK_NULL_HANDLE, &index);

            lock.lock();
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                generation.spare.push_back(semaphore);
                if (result != VK_ERROR_OUT_OF_DATE_KHR) {
                    return std::unexpected(detail::vulkan_error(result, "vkAcquireNextImageKHR", what));
                }

                generation.stale = true;
                continue;
            }

            auto& slot = generation.slots[index];
            if (slot.acquired != VK_NULL_HANDLE) {
                generation.spare.push_back(slot.acquired);
            }
            slot.acquired = semaphore;
            slot.signaled = 0;
            slot.acquisitions++;
            slot.awaited = false;
            slot.held = true;
            generation.held++;
            generation.stale = generation.stale || result == VK_SUBOPTIMAL_KHR;
            lock.unlock();

            auto state = std::make_unique<frame::state>();
            state->generation = self.current;
            state->index = index;

            return vk::frame{std::move(state)};
        }

        return std::unexpected(rei::error{
            vk::error_code::surface_unavailable,
            std::format("acquire from {}: the surface was out of date again after every rebuild", what),
        });
    }

    auto swapchain::present(vk::frame frame) -> std::expected<void, rei::error> {
        auto& self = *m_state;
        auto& context = *self.device;
        const auto what = detail::quoted(self.name, "swapchain");

        if (!frame.m_state) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("present to {}: the frame was moved from", what),
            });
        }

        auto& generation = *frame.m_state->generation;
        if (generation.device != self.device || generation.surface.handle() != self.surface.handle()) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("present to {}: the frame was acquired from {}", what,
                            detail::quoted(generation.name, "swapchain")),
            });
        }

        auto& slot = generation.slots[frame.m_state->index];

        const auto locks = std::scoped_lock{context.queue.mutex, generation.mutex};
        const auto info = VkPresentInfoKHR{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = slot.signaled,
            .pWaitSemaphores = slot.rendered.data(),
            .swapchainCount = 1,
            .pSwapchains = &generation.handle,
            .pImageIndices = &frame.m_state->index,
        };
        const auto result = context.table.vkQueuePresentKHR(context.queue.handle, &info);

        frame.m_state->presented = true;
        slot.held = false;
        generation.held--;
        generation.presented = context.queue.submitted + 1;

        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
            generation.stale = true;
        } else if (result != VK_SUCCESS) {
            return std::unexpected(detail::vulkan_error(result, "vkQueuePresentKHR", what));
        }

        return {};
    }

    auto swapchain::present_mode() const noexcept -> vk::present_mode {
        return m_state->mode;
    }

    auto swapchain::present_mode(vk::present_mode mode) noexcept -> void {
        m_state->mode = mode;
    }

    auto swapchain::format() const noexcept -> vk::format {
        return static_cast<vk::format>(m_state->format.format);
    }

    auto swapchain::extent() const noexcept -> vk::extent {
        return m_state->extent;
    }

    auto swapchain::handle() const noexcept -> VkSwapchainKHR {
        return m_state->current ? m_state->current->handle : VK_NULL_HANDLE;
    }
} // namespace rei::gpu::vk
