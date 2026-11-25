#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/vocabulary.hpp"

namespace volk {
    struct VolkInstanceTable;
} // namespace volk

namespace rei::gpu::vk {
    struct message {
        vk::severity severity;
        std::string_view id;
        std::string_view text;
    };

    struct physical_device {
        std::string name;
        vk::device_type type;
        VkPhysicalDevice handle;
    };

    struct instance_options {
        std::string application{};
        std::vector<std::string> extensions{};
        vk::validation validation{vk::validation::none};
        bool trace{false};
        std::function<void(const vk::message&)> on_message{};
    };

    class instance {
      private:
        friend class device;

        struct state;

        std::shared_ptr<instance::state> m_state;

        explicit instance(std::shared_ptr<instance::state> state) noexcept;

      public:
        static auto make(const vk::instance_options& options = {}) -> std::expected<vk::instance, rei::error>;

        [[nodiscard]] auto physical_devices() const -> std::vector<vk::physical_device>;
        [[nodiscard]] auto handle() const noexcept -> VkInstance;
        [[nodiscard]] auto raw() const noexcept -> const volk::VolkInstanceTable&;
    };
} // namespace rei::gpu::vk
