#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/gpu/vk/detail/features.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    namespace detail {
        auto info_of(vk::feature feature) noexcept -> const detail::feature_info& {
            return detail::feature_table[std::to_underlying(feature)];
        }

        feature_chain::feature_chain() {
            const auto root =
                VkPhysicalDeviceFeatures2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

            m_structs.emplace_back(sizeof(root));
            std::memcpy(m_structs.front().data(), &root, sizeof(root));
        }

        auto feature_chain::_find(VkStructureType structure) const noexcept -> std::optional<std::size_t> {
            for (auto i = 0uz; i < m_structs.size(); i++) {
                auto header = VkBaseOutStructure{};
                std::memcpy(&header, m_structs[i].data(), sizeof(header));

                if (header.sType == structure) {
                    return i;
                }
            }

            return std::nullopt;
        }

        auto feature_chain::add(const detail::feature_info& info) -> void {
            if (_find(info.structure)) {
                return;
            }

            auto root = VkBaseOutStructure{};
            std::memcpy(&root, m_structs.front().data(), sizeof(root));

            auto& added = m_structs.emplace_back(info.size);
            const auto header = VkBaseOutStructure{.sType = info.structure, .pNext = root.pNext};
            std::memcpy(added.data(), &header, sizeof(header));

            root.pNext = reinterpret_cast<VkBaseOutStructure*>(added.data());
            std::memcpy(m_structs.front().data(), &root, sizeof(root));
        }

        auto feature_chain::enable(const detail::feature_info& info) -> void {
            add(info);

            const auto enabled = VkBool32{VK_TRUE};
            std::memcpy(m_structs[*_find(info.structure)].data() + info.offset, &enabled, sizeof(enabled));
        }

        auto feature_chain::has(const detail::feature_info& info) const noexcept -> bool {
            const auto found = _find(info.structure);
            if (!found) {
                return false;
            }

            auto flag = VkBool32{};
            std::memcpy(&flag, m_structs[*found].data() + info.offset, sizeof(flag));
            return flag == VK_TRUE;
        }

        auto feature_chain::root() noexcept -> VkPhysicalDeviceFeatures2& {
            return *reinterpret_cast<VkPhysicalDeviceFeatures2*>(m_structs.front().data());
        }
    } // namespace detail

    auto to_string(vk::feature feature) noexcept -> std::string_view {
        return detail::info_of(feature).name;
    }

    auto to_feature(std::string_view name) noexcept -> std::optional<vk::feature> {
        const auto found = std::ranges::find(detail::feature_table, name, &detail::feature_info::name);
        if (found != detail::feature_table.end()) {
            return static_cast<vk::feature>(std::distance(detail::feature_table.begin(), found));
        }

        const auto alias = std::ranges::find(detail::feature_aliases, name, &detail::feature_alias::name);
        if (alias != detail::feature_aliases.end()) {
            return alias->feature;
        }

        return std::nullopt;
    }
} // namespace rei::gpu::vk
