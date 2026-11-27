#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace rei::gpu::vk {
    enum class error_code {
        loader_not_found = 1,
        no_suitable_device,
        missing_feature,
        missing_extension,
        missing_layer,
        invalid_argument,
        invalid_spirv,
        layout_mismatch,
        out_of_memory,
        device_lost,
        surface_lost,
        surface_unavailable,
        timed_out,
        driver_error,
    };

    namespace detail {
        class error_category final : public std::error_category {
          private:
          public:
            [[nodiscard]] auto name() const noexcept -> const char* override { return "rei.gpu.vk"; }

            [[nodiscard]] auto message(int code) const -> std::string override {
                switch (static_cast<vk::error_code>(code)) {
                case vk::error_code::loader_not_found:
                    return "Vulkan loader not found";
                case vk::error_code::no_suitable_device:
                    return "no suitable device";
                case vk::error_code::missing_feature:
                    return "device feature not supported";
                case vk::error_code::missing_extension:
                    return "extension not available";
                case vk::error_code::missing_layer:
                    return "layer not available";
                case vk::error_code::invalid_argument:
                    return "invalid argument";
                case vk::error_code::invalid_spirv:
                    return "invalid SPIR-V";
                case vk::error_code::layout_mismatch:
                    return "layout doesn't match the shader";
                case vk::error_code::out_of_memory:
                    return "out of memory";
                case vk::error_code::device_lost:
                    return "device lost";
                case vk::error_code::surface_lost:
                    return "surface lost";
                case vk::error_code::surface_unavailable:
                    return "surface unavailable";
                case vk::error_code::timed_out:
                    return "timed out";
                case vk::error_code::driver_error:
                    return "driver error";
                }

                return "unknown Vulkan error";
            }

            [[nodiscard]] auto default_error_condition(int code) const noexcept
                -> std::error_condition override {
                switch (static_cast<vk::error_code>(code)) {
                case vk::error_code::invalid_argument:
                    return std::make_error_condition(std::errc::invalid_argument);
                case vk::error_code::out_of_memory:
                    return std::make_error_condition(std::errc::not_enough_memory);
                case vk::error_code::timed_out:
                    return std::make_error_condition(std::errc::timed_out);
                default:
                    return {code, *this};
                }
            }
        };
    } // namespace detail

    inline auto make_error_code(vk::error_code code) noexcept -> std::error_code {
        static const detail::error_category category;
        return {static_cast<int>(code), category};
    }
} // namespace rei::gpu::vk

namespace std {
    template <> class is_error_code_enum<rei::gpu::vk::error_code> : public true_type {};
} // namespace std
