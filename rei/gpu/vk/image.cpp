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
        auto default_home(vk::image_usage usage) noexcept -> std::optional<vk::access> {
            if (vk::has(usage, vk::image_usage::storage)) {
                return vk::access::shader_write;
            }
            if (vk::has(usage, vk::image_usage::sampled)) {
                return vk::access::shader_read;
            }
            if (vk::has(usage, vk::image_usage::color_attachment)) {
                return vk::access::color_attachment;
            }
            if (vk::has(usage, vk::image_usage::depth_stencil_attachment)) {
                return vk::access::depth_attachment;
            }
            if (vk::has(usage, vk::image_usage::transfer_src)) {
                return vk::access::transfer_src;
            }
            if (vk::has(usage, vk::image_usage::transfer_dst)) {
                return vk::access::transfer_dst;
            }

            return std::nullopt;
        }

        auto misfit(vk::access home, vk::image_usage usage) noexcept -> std::string_view {
            switch (home) {
            case vk::access::shader_read:
                return vk::has(usage, vk::image_usage::sampled) ? "" : "needs the sampled usage";
            case vk::access::shader_write:
                return vk::has(usage, vk::image_usage::sampled) || vk::has(usage, vk::image_usage::storage)
                           ? ""
                           : "needs the sampled or storage usage";
            case vk::access::color_attachment:
                return vk::has(usage, vk::image_usage::color_attachment) ? ""
                                                                         : "needs the color_attachment usage";
            case vk::access::depth_attachment:
            case vk::access::depth_read:
                return vk::has(usage, vk::image_usage::depth_stencil_attachment)
                           ? ""
                           : "needs the depth_stencil_attachment usage";
            case vk::access::transfer_src:
                return vk::has(usage, vk::image_usage::transfer_src) ? "" : "needs the transfer_src usage";
            case vk::access::transfer_dst:
                return vk::has(usage, vk::image_usage::transfer_dst) ? "" : "needs the transfer_dst usage";
            case vk::access::manual:
                return "";
            case vk::access::undefined:
                return "isn't a place an image can stay";
            case vk::access::present:
                return "is for swapchain images";
            }

            return "is unknown";
        }

        auto viewable(vk::image_usage usage) noexcept -> bool {
            return vk::has(usage, vk::image_usage::sampled) || vk::has(usage, vk::image_usage::storage) ||
                   vk::has(usage, vk::image_usage::color_attachment) ||
                   vk::has(usage, vk::image_usage::depth_stencil_attachment);
        }

        auto check(const vk::image_options& options) -> std::expected<vk::access, rei::error> {
            const auto& extent = options.extent;
            const auto invalid = [&](std::string_view problem) -> std::unexpected<rei::error> {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: {}", detail::quoted(options.name, "image"), problem));
            };

            if (options.format == vk::format::undefined) {
                return invalid("format is undefined");
            }
            if (std::to_underlying(options.usage) == 0) {
                return invalid("usage is empty");
            }
            if (extent.width == 0 || extent.height == 0 || extent.depth == 0) {
                return invalid(std::format("extent {}x{}x{} has a side of 0", extent.width, extent.height,
                                           extent.depth));
            }
            if (options.array_layers == 0) {
                return invalid("array_layers is 0");
            }
            if (extent.depth > 1 && (options.array_layers > 1 || options.cube)) {
                return invalid("a 3D image can't have array layers or be a cube");
            }
            if (options.cube && (extent.width != extent.height || options.array_layers % 6 != 0)) {
                return invalid("a cube needs square sides and a multiple of 6 array layers");
            }
            if (options.mip_levels == 0 || options.mip_levels > vk::mip_count(extent)) {
                return invalid(std::format("{} mip levels, and {}x{}x{} has room for 1 to {}",
                                           options.mip_levels, extent.width, extent.height, extent.depth,
                                           vk::mip_count(extent)));
            }
            if (!std::has_single_bit(options.samples) || options.samples > 64) {
                return invalid(
                    std::format("{} samples, and that isn't a power of two up to 64", options.samples));
            }
            if (options.samples > 1 && (options.mip_levels > 1 || extent.depth > 1 || options.cube)) {
                return invalid("a multisampled image has one mip level, two dimensions and no cube");
            }

            const auto home = options.home ? options.home : default_home(options.usage);
            if (!home) {
                return invalid("usage has no bit rei knows");
            }
            if (const auto problem = misfit(*home, options.usage); !problem.empty()) {
                return invalid(std::format("home {} {}", detail::use_of(*home).name, problem));
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

        if (mips == 0 || options.first_mip >= source.mip_levels ||
            mips > source.mip_levels - options.first_mip || layers == 0 ||
            options.first_layer >= source.array_layers ||
            layers > source.array_layers - options.first_layer) {
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

        if (!viewable(image->usage)) {
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
