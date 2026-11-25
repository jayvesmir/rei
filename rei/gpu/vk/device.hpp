#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/instance.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace volk {
    struct VolkDeviceTable;
} // namespace volk

namespace rei::gpu::vk {
    class queue;

    struct ticket {
        std::uint32_t queue{};
        std::uint64_t value{};
    };

    struct device_options {
        std::vector<vk::feature> require{};
        std::vector<vk::feature> prefer{};
        std::vector<std::string> extensions{};
        std::optional<vk::physical_device> physical{};
        std::string name{};
    };

    class device {
      private:
        struct state;

        std::shared_ptr<device::state> m_state;

        explicit device(std::shared_ptr<device::state> state) noexcept;

      public:
        static auto make(const vk::instance& instance, const vk::device_options& options = {})
            -> std::expected<vk::device, rei::error>;

        [[nodiscard]] auto queue() const noexcept -> vk::queue;
        [[nodiscard]] auto has(vk::feature feature) const noexcept -> bool;
        [[nodiscard]] auto limits() const noexcept -> const VkPhysicalDeviceLimits&;
        [[nodiscard]] auto physical() const noexcept -> const vk::physical_device&;

        auto wait(vk::ticket ticket, std::optional<std::chrono::nanoseconds> timeout = {}) const
            -> std::expected<void, rei::error>;
        auto wait_idle() const -> std::expected<void, rei::error>;
        [[nodiscard]] auto done(vk::ticket ticket) const noexcept -> bool;
        auto collect() const -> void;

        [[nodiscard]] auto handle() const noexcept -> VkDevice;
        [[nodiscard]] auto raw() const noexcept -> const volk::VolkDeviceTable&;
    };

    class queue {
      private:
        friend class device;

        struct state;

        std::shared_ptr<queue::state> m_state;

        explicit queue(std::shared_ptr<queue::state> state) noexcept;

      public:
        [[nodiscard]] auto family() const noexcept -> std::uint32_t;
        [[nodiscard]] auto handle() const noexcept -> VkQueue;
    };
} // namespace rei::gpu::vk
