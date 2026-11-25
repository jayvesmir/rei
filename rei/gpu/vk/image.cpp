#include <algorithm>
#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/detail/access.hpp"
#include "rei/gpu/vk/detail/formats.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/image.hpp"

namespace rei::gpu::vk {
    namespace {
        auto check(const vk::image_options& options) -> std::expected<vk::access, rei::error> {
            const auto what = detail::quoted(options.name, "image");
            const auto& extent = options.extent;
            const auto usage = options.usage;

            if (options.format == vk::format::undefined) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: format is undefined", what));
            }
            if (std::to_underlying(usage) == 0) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: usage is empty", what));
            }
            if (extent.width == 0 || extent.height == 0 || extent.depth == 0) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: extent {}x{}x{} has a side of 0", what, extent.width,
                                                   extent.height, extent.depth));
            }
            if (options.array_layers == 0) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: array_layers is 0", what));
            }
            if (extent.depth > 1 && (options.array_layers > 1 || options.cube)) {
                return detail::failure(
                    vk::error_code::invalid_argument,
                    std::format("{}: a 3D image can't have array layers or be a cube", what));
            }
            if (options.cube && (extent.width != extent.height || options.array_layers % 6 != 0)) {
                return detail::failure(
                    vk::error_code::invalid_argument,
                    std::format("{}: a cube needs square sides and a multiple of 6 array layers", what));
            }
            if (options.mip_levels == 0 || options.mip_levels > vk::mip_count(extent)) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: {} mip levels, and {}x{}x{} has room for 1 to {}",
                                                   what, options.mip_levels, extent.width, extent.height,
                                                   extent.depth, vk::mip_count(extent)));
            }
            if (!std::has_single_bit(options.samples) || options.samples > 64) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: {} samples, and that isn't a power of two up to 64",
                                                   what, options.samples));
            }
            if (options.samples > 1 && (options.mip_levels > 1 || extent.depth > 1 || options.cube)) {
                return detail::failure(
                    vk::error_code::invalid_argument,
                    std::format("{}: a multisampled image has one mip level, two dimensions and no cube",
                                what));
            }

            auto home = options.home;
            if (!home) {
                if (vk::has(usage, vk::image_usage::storage)) {
                    home = vk::access::shader_write;
                } else if (vk::has(usage, vk::image_usage::sampled)) {
                    home = vk::access::shader_read;
                } else if (vk::has(usage, vk::image_usage::color_attachment)) {
                    home = vk::access::color_attachment;
                } else if (vk::has(usage, vk::image_usage::depth_stencil_attachment)) {
                    home = vk::access::depth_attachment;
                } else if (vk::has(usage, vk::image_usage::transfer_src)) {
                    home = vk::access::transfer_src;
                } else if (vk::has(usage, vk::image_usage::transfer_dst)) {
                    home = vk::access::transfer_dst;
                } else {
                    return detail::failure(vk::error_code::invalid_argument,
                                           std::format("{}: usage has no bit rei knows", what));
                }
            }

            auto problem = std::string_view{"is unknown"};
            switch (*home) {
            case vk::access::shader_read:
                problem = vk::has(usage, vk::image_usage::sampled) ? "" : "needs the sampled usage";
                break;
            case vk::access::shader_write:
                problem = vk::has(usage, vk::image_usage::sampled) || vk::has(usage, vk::image_usage::storage)
                              ? ""
                              : "needs the sampled or storage usage";
                break;
            case vk::access::color_attachment:
                problem = vk::has(usage, vk::image_usage::color_attachment)
                              ? ""
                              : "needs the color_attachment usage";
                break;
            case vk::access::depth_attachment:
            case vk::access::depth_read:
                problem = vk::has(usage, vk::image_usage::depth_stencil_attachment)
                              ? ""
                              : "needs the depth_stencil_attachment usage";
                break;
            case vk::access::transfer_src:
                problem = vk::has(usage, vk::image_usage::transfer_src) ? "" : "needs the transfer_src usage";
                break;
            case vk::access::transfer_dst:
                problem = vk::has(usage, vk::image_usage::transfer_dst) ? "" : "needs the transfer_dst usage";
                break;
            case vk::access::manual:
                problem = "";
                break;
            case vk::access::undefined:
                problem = "isn't a place an image can stay";
                break;
            case vk::access::present:
                problem = "is for swapchain images";
                break;
            }
            if (!problem.empty()) {
                return detail::failure(
                    vk::error_code::invalid_argument,
                    std::format("{}: home {} {}", what, detail::use_of(*home).name, problem));
            }

            return *home;
        }
    } // namespace

    auto image::state::range() const noexcept -> VkImageSubresourceRange {
        return {
            .aspectMask = detail::info_of(format).aspects,
            .baseMipLevel = 0,
            .levelCount = mip_levels,
            .baseArrayLayer = 0,
            .layerCount = array_layers,
        };
    }

    auto image::state::extent_of(std::uint32_t mip) const noexcept -> vk::extent {
        return {
            .width = std::max(extent.width >> mip, 1U),
            .height = std::max(extent.height >> mip, 1U),
            .depth = std::max(extent.depth >> mip, 1U),
        };
    }

    auto image::state::label(const VkImageSubresourceRange& range) const -> std::string {
        auto text = detail::label(name, "image");

        if (mip_levels != 1 && range.levelCount != mip_levels) {
            text += range.levelCount == 1 ? std::format(" mip {}", range.baseMipLevel)
                                          : std::format(" mips {}-{}", range.baseMipLevel,
                                                        range.baseMipLevel + range.levelCount - 1);
        }
        if (array_layers != 1 && range.layerCount != array_layers) {
            text += range.layerCount == 1 ? std::format(" layer {}", range.baseArrayLayer)
                                          : std::format(" layers {}-{}", range.baseArrayLayer,
                                                        range.baseArrayLayer + range.layerCount - 1);
        }

        return text;
    }

    image::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        device->retire({
            .label = detail::label(name, "image"),
            .image = handle,
            .allocation = allocation,
            .memory = memory,
        });
    }

    image_view::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        device->retire({.label = own ? std::string{} : detail::label(name, "image view"), .view = handle});
    }

    sampler::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        device->retire({.label = detail::label(name, "sampler"), .sampler = handle});
    }

    image_view::image_view(std::shared_ptr<image_view::state> state) noexcept : m_state(std::move(state)) {}

    auto image_view::make(const vk::image& image, const vk::image_view_options& options)
        -> std::expected<vk::image_view, rei::error> {
        const auto& source = *image.m_state;
        const auto mips =
            options.mip_levels.value_or(source.mip_levels - std::min(options.first_mip, source.mip_levels));
        const auto layers = options.array_layers.value_or(source.array_layers -
                                                          std::min(options.first_layer, source.array_layers));

        if (mips == 0 || !detail::fits(source.mip_levels, options.first_mip, mips) || layers == 0 ||
            !detail::fits(source.array_layers, options.first_layer, layers)) {
            return detail::failure(
                vk::error_code::invalid_argument,
                std::format(
                    "{}: {} mip levels from {} and {} array layers from {} don't fit in {}, which has {} "
                    "and {}",
                    detail::quoted(options.name, "image view"), mips, options.first_mip, layers,
                    options.first_layer, detail::quoted(source.name, "image"), source.mip_levels,
                    source.array_layers));
        }

        const auto range = VkImageSubresourceRange{
            .aspectMask = detail::info_of(source.format).aspects,
            .baseMipLevel = options.first_mip,
            .levelCount = mips,
            .baseArrayLayer = options.first_layer,
            .layerCount = layers,
        };
        return image::_view(image.m_state, range, options.name, false);
    }

    auto image_view::format() const noexcept -> vk::format {
        return m_state->format;
    }

    auto image_view::extent() const noexcept -> vk::extent {
        return m_state->extent;
    }

    auto image_view::handle() const noexcept -> VkImageView {
        return m_state->handle;
    }

    image::image(std::shared_ptr<image::state> state, vk::image_view view) noexcept
        : m_state(std::move(state)), m_view(std::move(view)) {}

    auto image::_view(const std::shared_ptr<image::state>& image, const VkImageSubresourceRange& range,
                      std::string name, bool own) -> std::expected<vk::image_view, rei::error> {
        auto& context = *image->device;

        auto state = std::make_shared<image_view::state>();
        state->device = image->device;
        state->image = image;
        state->image_name = image->name;
        state->format = image->format;
        state->extent = image->extent_of(range.baseMipLevel);
        state->range = range;
        state->name = std::move(name);
        state->own = own;

        if (!vk::has(image->usage, vk::image_usage::sampled) &&
            !vk::has(image->usage, vk::image_usage::storage) &&
            !vk::has(image->usage, vk::image_usage::color_attachment) &&
            !vk::has(image->usage, vk::image_usage::depth_stencil_attachment)) {
            if (own) {
                return vk::image_view{std::move(state)};
            }

            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: the usage of {} allows no view",
                                               detail::quoted(state->name, "image view"),
                                               detail::quoted(image->name, "image")));
        }

        auto type = VK_IMAGE_VIEW_TYPE_2D;
        if (image->extent.depth > 1) {
            type = VK_IMAGE_VIEW_TYPE_3D;
        } else if (image->cube && range.layerCount % 6 == 0) {
            type = range.layerCount == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        } else if (range.layerCount > 1) {
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        }

        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY &&
            !context.features[std::to_underlying(vk::feature::image_cube_array)]) {
            return detail::failure(
                vk::error_code::missing_feature,
                std::format("{}: a view of {} cube layers needs the feature image_cube_array",
                            detail::quoted(state->name, "image view"), range.layerCount));
        }

        const auto info = VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image->handle,
            .viewType = type,
            .format = static_cast<VkFormat>(image->format),
            .subresourceRange = range,
        };
        if (const auto result =
                context.table.vkCreateImageView(context.handle, &info, nullptr, &state->handle);
            result != VK_SUCCESS) {
            return detail::failure(result, "vkCreateImageView");
        }

        context.name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(state->handle), state->name);

        return vk::image_view{std::move(state)};
    }

    auto image::_complete(const vk::image_options& options, vk::access home,
                          std::shared_ptr<image::state> state) -> std::expected<vk::image, rei::error> {
        auto& context = *state->device;

        state->format = options.format;
        state->extent = options.extent;
        state->usage = options.usage;
        state->mip_levels = options.mip_levels;
        state->array_layers = options.array_layers;
        state->samples = options.samples;
        state->cube = options.cube;
        state->home = home;
        state->name = options.name;

        context.name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(state->handle), options.name);

        auto view = image::_view(state, state->range(), options.name, true);
        if (!view) {
            return std::unexpected(view.error());
        }

        if (home != vk::access::manual) {
            const auto lock = std::scoped_lock{context.mutex};
            std::erase_if(context.new_images,
                          [](const std::weak_ptr<image::state>& image) -> bool { return image.expired(); });
            context.new_images.push_back(state);
        }

        return vk::image{std::move(state), std::move(*view)};
    }

    auto image::make(const vk::device& device, const vk::image_options& options)
        -> std::expected<vk::image, rei::error> {
        const auto home = check(options);
        if (!home) {
            return std::unexpected(home.error());
        }

        const auto& context = *device.m_state;
        const auto& extent = options.extent;
        const auto info = VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags =
                options.cube ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT} : VkImageCreateFlags{},
            .imageType = extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = static_cast<VkFormat>(options.format),
            .extent = {.width = extent.width, .height = extent.height, .depth = extent.depth},
            .mipLevels = options.mip_levels,
            .arrayLayers = options.array_layers,
            .samples = static_cast<VkSampleCountFlagBits>(options.samples),
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = static_cast<VkImageUsageFlags>(options.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        const auto what = detail::quoted(options.name, "image");
        auto limits = VkImageFormatProperties{};
        const auto supported = context.instance.raw().vkGetPhysicalDeviceImageFormatProperties(
            context.physical.handle, info.format, info.imageType, info.tiling, info.usage, info.flags,
            &limits);
        if (supported == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: '{}' doesn't support {} images with this usage", what,
                                               context.physical.name, detail::info_of(options.format).name));
        }
        if (supported != VK_SUCCESS) {
            return detail::failure(supported, "vkGetPhysicalDeviceImageFormatProperties");
        }
        if (extent.width > limits.maxExtent.width || extent.height > limits.maxExtent.height ||
            extent.depth > limits.maxExtent.depth || options.mip_levels > limits.maxMipLevels ||
            options.array_layers > limits.maxArrayLayers || (limits.sampleCounts & options.samples) == 0) {
            return detail::failure(
                vk::error_code::invalid_argument,
                std::format("{}: '{}' allows {} images of up to {}x{}x{}, {} mip levels, {} array layers and "
                            "sample counts {:#x}",
                            what, context.physical.name, detail::info_of(options.format).name,
                            limits.maxExtent.width, limits.maxExtent.height, limits.maxExtent.depth,
                            limits.maxMipLevels, limits.maxArrayLayers, limits.sampleCounts));
        }

        auto state = std::make_shared<image::state>();
        state->device = device.m_state;

        const auto allocation = VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO};
        if (const auto result = vmaCreateImage(context.allocator, &info, &allocation, &state->handle,
                                               &state->allocation, nullptr);
            result != VK_SUCCESS) {
            const auto error = detail::failure(result, "vmaCreateImage").error();
            return detail::failure(error.code(), std::format("{}: {}", what, error.detail()));
        }

        return image::_complete(options, *home, std::move(state));
    }

    auto image::adopt(const vk::device& device, VkImage handle, VkDeviceMemory memory,
                      const vk::image_options& options) -> std::expected<vk::image, rei::error> {
        const auto home = check(options);
        if (!home) {
            return std::unexpected(home.error());
        }

        auto state = std::make_shared<image::state>();
        state->device = device.m_state;
        state->handle = handle;
        state->memory = memory;

        return image::_complete(options, *home, std::move(state));
    }

    image::operator const vk::image_view&() const noexcept {
        return m_view;
    }

    auto image::view() const noexcept -> const vk::image_view& {
        return m_view;
    }

    auto image::format() const noexcept -> vk::format {
        return m_state->format;
    }

    auto image::extent() const noexcept -> vk::extent {
        return m_state->extent;
    }

    auto image::home() const noexcept -> vk::access {
        return m_state->home;
    }

    auto image::handle() const noexcept -> VkImage {
        return m_state->handle;
    }

    auto mip_count(vk::extent extent) noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(
            std::bit_width(std::max({extent.width, extent.height, extent.depth})));
    }

    sampler::sampler(std::shared_ptr<const sampler::state> state) noexcept : m_state(std::move(state)) {}

    auto sampler::make(const vk::device& device, const vk::sampler_options& options)
        -> std::expected<vk::sampler, rei::error> {
        const auto what = detail::quoted(options.name, "sampler");

        if (options.anisotropy && !device.has(vk::feature::sampler_anisotropy)) {
            return detail::failure(vk::error_code::missing_feature,
                                   std::format("{}: anisotropy needs the feature sampler_anisotropy", what));
        }
        if (options.anisotropy &&
            (*options.anisotropy < 1.0f || *options.anisotropy > device.limits().maxSamplerAnisotropy)) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: anisotropy {} is outside 1 to {}", what,
                                               *options.anisotropy, device.limits().maxSamplerAnisotropy));
        }

        const auto address = static_cast<VkSamplerAddressMode>(options.address);
        const auto info = VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = static_cast<VkFilter>(options.mag),
            .minFilter = static_cast<VkFilter>(options.min),
            .mipmapMode = options.mip == vk::filter::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                             : VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = address,
            .addressModeV = address,
            .addressModeW = address,
            .anisotropyEnable = options.anisotropy ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = options.anisotropy.value_or(1.0f),
            .compareEnable = options.compare ? VK_TRUE : VK_FALSE,
            .compareOp = static_cast<VkCompareOp>(options.compare.value_or(vk::compare_op::never)),
            .maxLod = VK_LOD_CLAMP_NONE,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        };

        auto state = std::make_shared<sampler::state>();
        state->device = device.m_state;
        state->name = options.name;

        auto& context = *state->device;
        if (const auto result = context.table.vkCreateSampler(context.handle, &info, nullptr, &state->handle);
            result != VK_SUCCESS) {
            return detail::failure(result, "vkCreateSampler");
        }

        context.name(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<std::uint64_t>(state->handle), options.name);

        return vk::sampler{std::move(state)};
    }

    auto sampler::handle() const noexcept -> VkSampler {
        return m_state->handle;
    }
} // namespace rei::gpu::vk
