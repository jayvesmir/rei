#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/buffer.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"

namespace rei::gpu::vk {
    namespace {
        auto check(const vk::device& device, const vk::buffer_options& options)
            -> std::expected<void, rei::error> {
            const auto what = detail::quoted(options.name, "buffer");

            if (options.size == 0) {
                return detail::failure(vk::error_code::invalid_argument, std::format("{}: size is 0", what));
            }
            if (std::to_underlying(options.usage) == 0) {
                return detail::failure(vk::error_code::invalid_argument,
                                       std::format("{}: usage is empty", what));
            }
            if (vk::has(options.usage, vk::buffer_usage::shader_device_address) &&
                !device.has(vk::feature::buffer_device_address)) {
                return detail::failure(
                    vk::error_code::missing_feature,
                    std::format("{}: shader_device_address needs the feature buffer_device_address", what));
            }

            return {};
        }

        auto fits(std::uint64_t size, std::uint64_t offset, std::uint64_t count) noexcept -> bool {
            return offset <= size && count <= size - offset;
        }
    } // namespace

    buffer::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        device->retire({
            .label = detail::label(name, "buffer"),
            .buffer = handle,
            .allocation = allocation,
            .memory = memory,
        });
    }

    buffer::buffer(std::shared_ptr<buffer::state> state) noexcept : m_state(std::move(state)) {}

    auto buffer::make(const vk::device& device, const vk::buffer_options& options)
        -> std::expected<vk::buffer, rei::error> {
        if (const auto checked = check(device, options); !checked) {
            return std::unexpected(checked.error());
        }

        auto state = std::make_shared<buffer::state>();
        state->device = device.m_state;
        state->size = options.size;
        state->usage = options.usage;
        state->host = options.host;
        state->name = options.name;

        const auto info = VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = options.size,
            .usage = static_cast<VkBufferUsageFlags>(options.usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        auto flags = VmaAllocationCreateFlags{};
        if (options.host == vk::host_access::write) {
            flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        } else if (options.host == vk::host_access::read_write) {
            flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        const auto allocation_info = VmaAllocationCreateInfo{.flags = flags, .usage = VMA_MEMORY_USAGE_AUTO};

        auto allocated = VmaAllocationInfo{};
        if (const auto result = vmaCreateBuffer(state->device->allocator, &info, &allocation_info,
                                                &state->handle, &state->allocation, &allocated);
            result != VK_SUCCESS) {
            const auto error = detail::failure(result, "vmaCreateBuffer").error();
            return detail::failure(
                error.code(), std::format("{}: {}", detail::quoted(options.name, "buffer"), error.detail()));
        }

        if (options.host != vk::host_access::none) {
            state->mapped = std::span{static_cast<std::byte*>(allocated.pMappedData), options.size};
        }

        if (vk::has(options.usage, vk::buffer_usage::shader_device_address)) {
            const auto address = VkBufferDeviceAddressInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = state->handle,
            };
            state->address = state->device->table.vkGetBufferDeviceAddress(state->device->handle, &address);
        }

        state->device->name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(state->handle),
                            options.name);
        return vk::buffer{std::move(state)};
    }

    auto buffer::adopt(const vk::device& device, VkBuffer handle, VkDeviceMemory memory,
                       const vk::buffer_options& options) -> std::expected<vk::buffer, rei::error> {
        if (const auto checked = check(device, options); !checked) {
            return std::unexpected(checked.error());
        }
        if (options.host != vk::host_access::none) {
            return detail::failure(
                vk::error_code::invalid_argument,
                std::format("{}: an adopted buffer has no host access, map its memory yourself",
                            detail::quoted(options.name, "buffer")));
        }

        auto state = std::make_shared<buffer::state>();
        state->device = device.m_state;
        state->handle = handle;
        state->memory = memory;
        state->size = options.size;
        state->usage = options.usage;
        state->name = options.name;

        if (vk::has(options.usage, vk::buffer_usage::shader_device_address)) {
            const auto address = VkBufferDeviceAddressInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = handle,
            };
            state->address = state->device->table.vkGetBufferDeviceAddress(state->device->handle, &address);
        }

        state->device->name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(handle), options.name);
        return vk::buffer{std::move(state)};
    }

    auto buffer::write(std::span<const std::byte> data, std::uint64_t offset)
        -> std::expected<void, rei::error> {
        auto& state = *m_state;
        const auto what = std::format("write into {}", detail::quoted(state.name, "buffer"));

        if (state.host == vk::host_access::none) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: it has no host access", what));
        }
        if (!fits(state.size, offset, data.size())) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: {} bytes at offset {} go past its end at {}", what,
                                               data.size(), offset, state.size));
        }

        std::ranges::copy(data, state.mapped.subspan(offset).begin());

        if (const auto result =
                vmaFlushAllocation(state.device->allocator, state.allocation, offset, data.size());
            result != VK_SUCCESS) {
            return detail::failure(result, "vmaFlushAllocation");
        }

        return {};
    }

    auto buffer::read(std::span<std::byte> data, std::uint64_t offset) const
        -> std::expected<void, rei::error> {
        const auto& state = *m_state;
        const auto what = std::format("read from {}", detail::quoted(state.name, "buffer"));

        if (state.host != vk::host_access::read_write) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: reading needs host_access::read_write", what));
        }
        if (!fits(state.size, offset, data.size())) {
            return detail::failure(vk::error_code::invalid_argument,
                                   std::format("{}: {} bytes at offset {} go past its end at {}", what,
                                               data.size(), offset, state.size));
        }

        if (const auto result =
                vmaInvalidateAllocation(state.device->allocator, state.allocation, offset, data.size());
            result != VK_SUCCESS) {
            return detail::failure(result, "vmaInvalidateAllocation");
        }

        std::ranges::copy(state.mapped.subspan(offset, data.size()), data.begin());
        return {};
    }

    auto buffer::bytes() noexcept -> std::span<std::byte> {
        return m_state->mapped;
    }

    auto buffer::flush() -> std::expected<void, rei::error> {
        const auto& state = *m_state;

        if (state.host == vk::host_access::none) {
            return detail::failure(
                vk::error_code::invalid_argument,
                std::format("flush of {}: it has no host access", detail::quoted(state.name, "buffer")));
        }

        if (const auto result =
                vmaFlushAllocation(state.device->allocator, state.allocation, 0, VK_WHOLE_SIZE);
            result != VK_SUCCESS) {
            return detail::failure(result, "vmaFlushAllocation");
        }

        return {};
    }

    auto buffer::size() const noexcept -> std::uint64_t {
        return m_state->size;
    }

    auto buffer::address() const noexcept -> std::optional<std::uint64_t> {
        return m_state->address;
    }

    auto buffer::handle() const noexcept -> VkBuffer {
        return m_state->handle;
    }
} // namespace rei::gpu::vk
