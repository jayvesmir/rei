#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/device.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace rei::gpu::vk {
    struct buffer_options {
        std::uint64_t size;
        vk::buffer_usage usage;
        vk::host_access host{vk::host_access::none};
        std::string name{};
    };

    class buffer {
      private:
        friend class commands;
        friend class descriptor_set;
        friend class set_layout;

        struct state;

        std::shared_ptr<buffer::state> m_state;

        explicit buffer(std::shared_ptr<buffer::state> state) noexcept;

      public:
        static auto make(const vk::device& device, const vk::buffer_options& options)
            -> std::expected<vk::buffer, rei::error>;
        static auto adopt(const vk::device& device, VkBuffer handle, VkDeviceMemory memory,
                          const vk::buffer_options& options) -> std::expected<vk::buffer, rei::error>;

        buffer(buffer&& other) noexcept = default;
        auto operator=(buffer&& other) noexcept -> buffer& = default;

        auto write(std::span<const std::byte> data, std::uint64_t offset = 0)
            -> std::expected<void, rei::error>;
        auto read(std::span<std::byte> data, std::uint64_t offset = 0) const
            -> std::expected<void, rei::error>;

        [[nodiscard]] auto bytes() noexcept -> std::span<std::byte>;
        auto flush() -> std::expected<void, rei::error>;

        [[nodiscard]] auto size() const noexcept -> std::uint64_t;
        [[nodiscard]] auto address() const noexcept -> std::optional<std::uint64_t>;
        [[nodiscard]] auto handle() const noexcept -> VkBuffer;
    };
} // namespace rei::gpu::vk
