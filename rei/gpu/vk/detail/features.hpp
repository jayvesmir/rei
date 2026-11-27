#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/feature.hpp"

namespace rei::gpu::vk::detail {
    struct extension_requirement {
        std::string_view name;
        bool is_instance;
    };

    struct feature_info {
        std::string_view name;
        VkStructureType structure;
        std::size_t size;
        std::size_t offset;
        std::span<const detail::extension_requirement> extensions;
    };

    struct feature_alias {
        std::string_view name;
        vk::feature feature;
    };

    struct device_properties {
        VkPhysicalDeviceVulkan11Properties vulkan_11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        VkPhysicalDeviceVulkan12Properties vulkan_12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    };

    struct extension_enable {
        std::string_view name;
    };

    struct property_enable {
        std::string_view name;
        std::size_t offset;
        std::uint32_t mask;
    };

    using capability_enable = std::variant<vk::feature, detail::extension_enable, detail::property_enable>;

    struct capability_info {
        std::uint32_t value;
        std::string_view name;
        std::span<const detail::capability_enable> enables;
    };

    extern const std::span<const detail::feature_info> feature_table;
    extern const std::span<const detail::feature_alias> feature_aliases;
    extern const std::span<const detail::capability_info> capability_table;

    class feature_chain {
      private:
        std::vector<std::vector<std::byte>> m_structs;

        [[nodiscard]] auto _find(VkStructureType structure) const noexcept -> std::optional<std::size_t>;

      public:
        feature_chain();

        auto add(const detail::feature_info& info) -> void;
        auto enable(const detail::feature_info& info) -> void;

        [[nodiscard]] auto has(const detail::feature_info& info) const noexcept -> bool;
        [[nodiscard]] auto root() noexcept -> VkPhysicalDeviceFeatures2&;
    };
} // namespace rei::gpu::vk::detail
