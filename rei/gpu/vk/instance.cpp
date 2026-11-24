#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "rei/core/error.hpp"
#include "rei/gpu/vk/detail/loader.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/instance.hpp"

namespace rei::gpu::vk {
    namespace {
        constexpr std::string_view validation_layer{"VK_LAYER_KHRONOS_validation"};
        constexpr std::string_view debug_utils{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        constexpr std::string_view layer_settings{VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};

        constexpr auto synchronization_settings = std::to_array<std::string_view>({
            "validate_sync",
            "syncval_shader_accesses_heuristic",
            "syncval_load_op_after_store_op_validation",
        });

        auto extensions_of(std::string_view layer)
            -> std::expected<std::vector<VkExtensionProperties>, rei::error> {
            return detail::enumerate<VkExtensionProperties>(
                "vkEnumerateInstanceExtensionProperties",
                [&](std::uint32_t* count, VkExtensionProperties* items) -> VkResult {
                    return volk::vkEnumerateInstanceExtensionProperties(
                        layer.empty() ? nullptr : layer.data(), count, items);
                });
        }

        auto available_extensions(bool validating)
            -> std::expected<std::vector<VkExtensionProperties>, rei::error> {
            auto available = extensions_of({});
            if (!available || !validating) {
                return available;
            }

            const auto layers = detail::enumerate<VkLayerProperties>(
                "vkEnumerateInstanceLayerProperties",
                [](std::uint32_t* count, VkLayerProperties* items) -> VkResult {
                    return volk::vkEnumerateInstanceLayerProperties(count, items);
                });
            if (!layers) {
                return std::unexpected(layers.error());
            }

            if (!std::ranges::contains(*layers, validation_layer,
                                       [](const VkLayerProperties& layer) -> std::string_view {
                                           return std::string_view{layer.layerName};
                                       })) {
                return detail::failure(vk::error_code::missing_layer,
                                       std::format("the layer {} is not installed", validation_layer));
            }

            const auto provided = extensions_of(validation_layer);
            if (!provided) {
                return std::unexpected(provided.error());
            }

            available->append_range(*provided);
            return available;
        }

        auto enumerate_devices(const volk::VolkInstanceTable& table, VkInstance instance)
            -> std::expected<std::vector<vk::physical_device>, rei::error> {
            const auto describe = [&](VkPhysicalDevice handle) -> vk::physical_device {
                auto properties = VkPhysicalDeviceProperties{};
                table.vkGetPhysicalDeviceProperties(handle, &properties);

                return {
                    .name = properties.deviceName,
                    .type = static_cast<vk::device_type>(properties.deviceType),
                    .handle = handle,
                };
            };

            const auto handles = detail::enumerate<VkPhysicalDevice>(
                "vkEnumeratePhysicalDevices", [&](std::uint32_t* count, VkPhysicalDevice* items) -> VkResult {
                    return table.vkEnumeratePhysicalDevices(instance, count, items);
                });
            if (!handles) {
                return std::unexpected(handles.error());
            }

            return *handles | std::views::transform(describe) | std::ranges::to<std::vector>();
        }

        auto print_message(const vk::message& message) -> void {
            std::println(stderr, "vulkan {}: {}{}{}",
                         message.severity == vk::severity::error ? "error" : "warning", message.id,
                         message.id.empty() ? "" : ": ", message.text);
        }

        auto to_severity(VkDebugUtilsMessageSeverityFlagBitsEXT severity) noexcept -> vk::severity {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                return vk::severity::error;
            }
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                return vk::severity::warning;
            }
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
                return vk::severity::info;
            }

            return vk::severity::verbose;
        }

        VKAPI_ATTR auto VKAPI_CALL forward_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                   void* user) -> VkBool32 {
            const auto& on_message = *static_cast<const std::function<void(const vk::message&)>*>(user);
            on_message({
                .severity = to_severity(severity),
                .id = data->pMessageIdName != nullptr ? data->pMessageIdName : "",
                .text = data->pMessage,
            });

            return VK_FALSE;
        }
    } // namespace

    instance::state::~state() {
        if (handle == VK_NULL_HANDLE) {
            return;
        }

        if (messenger != VK_NULL_HANDLE) {
            table.vkDestroyDebugUtilsMessengerEXT(handle, messenger, nullptr);
        }
        table.vkDestroyInstance(handle, nullptr);
    }

    instance::instance(std::shared_ptr<instance::state> state) noexcept : m_state(std::move(state)) {}

    auto instance::make(const vk::instance_options& options) -> std::expected<vk::instance, rei::error> {
        if (!detail::load_loader()) {
            return detail::failure(vk::error_code::loader_not_found,
                                   "the Vulkan loader library was not found");
        }

        const auto validating = options.validation != vk::validation::none;
        const auto synchronizing = options.validation == vk::validation::full;

        const auto available = available_extensions(validating);
        if (!available) {
            return std::unexpected(available.error());
        }
        if (synchronizing && !detail::has_extension(*available, layer_settings)) {
            return detail::failure(vk::error_code::missing_extension,
                                   std::format("{} is too old for validation::full, it lacks {}",
                                               validation_layer, layer_settings));
        }

        auto state = std::make_shared<instance::state>();
        state->on_message = options.on_message ? options.on_message : print_message;

        const auto enable = [&](std::string_view name) -> void {
            if (!std::ranges::contains(state->extensions, name)) {
                state->extensions.emplace_back(name);
            }
        };
        for (const auto& name : options.extensions) {
            if (!detail::has_extension(*available, name)) {
                return detail::failure(vk::error_code::missing_extension,
                                       std::format("the instance extension {} is not available", name));
            }
            enable(name);
        }

        const auto debugging = detail::has_extension(*available, debug_utils);
        const auto reporting = debugging && (validating || options.on_message);
        if (debugging) {
            enable(debug_utils);
        }
        if (synchronizing) {
            enable(layer_settings);
        }

        const auto enabled = VkBool32{VK_TRUE};
        const auto turned_on = [&](std::string_view setting) -> VkLayerSettingEXT {
            return {
                .pLayerName = validation_layer.data(),
                .pSettingName = setting.data(),
                .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                .valueCount = 1,
                .pValues = &enabled,
            };
        };
        const auto settings =
            synchronization_settings | std::views::transform(turned_on) | std::ranges::to<std::vector>();
        const auto layer_configuration = VkLayerSettingsCreateInfoEXT{
            .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
            .settingCount = static_cast<std::uint32_t>(settings.size()),
            .pSettings = settings.data(),
        };

        const auto problems = VkDebugUtilsMessageSeverityFlagsEXT{
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        };
        const auto everything = problems | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        auto messenger = VkDebugUtilsMessengerCreateInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = synchronizing ? &layer_configuration : nullptr,
            .messageSeverity = options.on_message ? everything : problems,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = forward_message,
            .pUserData = &state->on_message,
        };

        const auto application = VkApplicationInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = options.application.c_str(),
            .pEngineName = "rei",
            .apiVersion = VK_API_VERSION_1_4,
        };
        const auto* const layer_name = validation_layer.data();
        const auto extension_names =
            state->extensions |
            std::views::transform([](const std::string& name) -> const char* { return name.c_str(); }) |
            std::ranges::to<std::vector>();
        const auto info = VkInstanceCreateInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = reporting ? &messenger : messenger.pNext,
            .pApplicationInfo = &application,
            .enabledLayerCount = validating ? 1U : 0U,
            .ppEnabledLayerNames = &layer_name,
            .enabledExtensionCount = static_cast<std::uint32_t>(extension_names.size()),
            .ppEnabledExtensionNames = extension_names.data(),
        };

        const auto result = volk::vkCreateInstance(&info, nullptr, &state->handle);
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
            return detail::failure(vk::error_code::no_suitable_device,
                                   "no Vulkan driver was found, vkCreateInstance returned " +
                                       detail::result_name(result));
        }
        if (result != VK_SUCCESS) {
            return detail::failure(result, "vkCreateInstance");
        }
        detail::load(state->table, state->handle);

        if (reporting) {
            messenger.pNext = nullptr;
            const auto created = state->table.vkCreateDebugUtilsMessengerEXT(state->handle, &messenger,
                                                                             nullptr, &state->messenger);
            if (created != VK_SUCCESS) {
                return detail::failure(created, "vkCreateDebugUtilsMessengerEXT");
            }
        }

        auto devices = enumerate_devices(state->table, state->handle);
        if (!devices) {
            return std::unexpected(devices.error());
        }
        state->physical_devices = std::move(*devices);

        return vk::instance{std::move(state)};
    }

    auto instance::physical_devices() const -> std::vector<vk::physical_device> {
        return m_state->physical_devices;
    }

    auto instance::handle() const noexcept -> VkInstance {
        return m_state->handle;
    }

    auto instance::raw() const noexcept -> const volk::VolkInstanceTable& {
        return m_state->table;
    }
} // namespace rei::gpu::vk
