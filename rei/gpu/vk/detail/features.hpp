#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
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

    extern const std::span<const detail::feature_info> feature_table;
    extern const std::span<const detail::feature_alias> feature_aliases;

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
