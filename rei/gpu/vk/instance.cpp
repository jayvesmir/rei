#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

        auto print_message(const vk::message& message) -> void {
            auto severity = std::string_view{"message"};
            switch (message.severity) {
            case vk::severity::trace:
                severity = "trace";
                break;
            case vk::severity::verbose:
                severity = "verbose";
                break;
            case vk::severity::info:
                severity = "info";
                break;
            case vk::severity::warning:
                severity = "warning";
                break;
            case vk::severity::error:
                severity = "error";
                break;
            }

            std::println(stderr, "vulkan {}: {}{}{}", severity, message.id, message.id.empty() ? "" : ": ",
                         message.text);
        }

        VKAPI_ATTR auto VKAPI_CALL forward_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT types,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                   void* user) -> VkBool32 {
            auto converted = vk::severity::verbose;
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                converted = vk::severity::error;
            } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                converted = vk::severity::warning;
            } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
                converted = vk::severity::info;
            }

            const auto& on_message = *static_cast<const std::function<void(const vk::message&)>*>(user);
            on_message({
                .severity = converted,
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

        auto available = detail::enumerate<VkExtensionProperties>(
            "vkEnumerateInstanceExtensionProperties",
            [](std::uint32_t* count, VkExtensionProperties* items) -> VkResult {
                return volk::vkEnumerateInstanceExtensionProperties(nullptr, count, items);
            });
        if (!available) {
            return std::unexpected(available.error());
        }

        if (validating) {
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

            const auto provided = detail::enumerate<VkExtensionProperties>(
                "vkEnumerateInstanceExtensionProperties",
                [](std::uint32_t* count, VkExtensionProperties* items) -> VkResult {
                    return volk::vkEnumerateInstanceExtensionProperties(validation_layer.data(), count,
                                                                        items);
                });
            if (!provided) {
                return std::unexpected(provided.error());
            }

            available->append_range(*provided);
        }

        if (synchronizing && !detail::has_extension(*available, layer_settings)) {
            return detail::failure(vk::error_code::missing_extension,
                                   std::format("{} is too old for validation::full, it lacks {}",
                                               validation_layer, layer_settings));
        }

        auto state = std::make_shared<instance::state>();
        state->on_message = options.on_message ? options.on_message : print_message;
        state->validation = options.validation;

        const auto* const variable = std::getenv("REI_GPU_VK_TRACE");
        state->trace = options.trace || (variable != nullptr && std::string_view{variable} == "1");

        for (const auto& name : options.extensions) {
            if (!detail::has_extension(*available, name)) {
                return detail::failure(vk::error_code::missing_extension,
                                       std::format("the instance extension {} is not available", name));
            }
            if (!std::ranges::contains(state->extensions, name)) {
                state->extensions.push_back(name);
            }
        }

        const auto debugging = detail::has_extension(*available, debug_utils);
        const auto reporting = debugging && (validating || options.on_message);
        if (debugging && !std::ranges::contains(state->extensions, debug_utils)) {
            state->extensions.emplace_back(debug_utils);
        }
        if (synchronizing && !std::ranges::contains(state->extensions, layer_settings)) {
            state->extensions.emplace_back(layer_settings);
        }

        const auto enabled = VkBool32{VK_TRUE};
        const auto settings = synchronization_settings |
                              std::views::transform([&](std::string_view setting) -> VkLayerSettingEXT {
                                  return {
                                      .pLayerName = validation_layer.data(),
                                      .pSettingName = setting.data(),
                                      .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                                      .valueCount = 1,
                                      .pValues = &enabled,
                                  };
                              }) |
                              std::ranges::to<std::vector>();
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

        const auto handles = detail::enumerate<VkPhysicalDevice>(
            "vkEnumeratePhysicalDevices", [&](std::uint32_t* count, VkPhysicalDevice* items) -> VkResult {
                return state->table.vkEnumeratePhysicalDevices(state->handle, count, items);
            });
        if (!handles) {
            return std::unexpected(handles.error());
        }

        state->physical_devices = *handles |
                                  std::views::transform([&](VkPhysicalDevice handle) -> vk::physical_device {
                                      auto properties = VkPhysicalDeviceProperties{};
                                      state->table.vkGetPhysicalDeviceProperties(handle, &properties);

                                      return {
                                          .name = properties.deviceName,
                                          .type = static_cast<vk::device_type>(properties.deviceType),
                                          .handle = handle,
                                      };
                                  }) |
                                  std::ranges::to<std::vector>();

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
