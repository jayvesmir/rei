#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "spirv-tools/libspirv.hpp"
#include "spirv_reflect/spirv_reflect.h"

#include "rei/core/error.hpp"
#include "rei/gpu/vk/detail/state.hpp"
#include "rei/gpu/vk/error_code.hpp"
#include "rei/gpu/vk/shader.hpp"

namespace rei::gpu::vk {
    namespace {
        constexpr auto reflect_results = std::to_array<std::string_view>({
            "success",
            "not ready",
            "parse failed",
            "allocation failed",
            "range exceeded",
            "null pointer",
            "internal error",
            "count mismatch",
            "element not found",
            "invalid code size",
            "invalid magic number",
            "unexpected end of file",
            "invalid ID reference",
            "set number overflow",
            "invalid storage class",
            "recursion",
            "invalid instruction",
            "unexpected block data",
            "invalid block member reference",
            "invalid entry point",
            "invalid execution mode",
            "maximum recursion exceeded",
        });
    } // namespace

    shader::shader(std::shared_ptr<const shader::state> state) noexcept : m_state(std::move(state)) {}

    auto shader::make(std::span<const std::uint32_t> spirv, std::string_view entry_point)
        -> std::expected<vk::shader, rei::error> {
        auto problem = std::string{};
        auto validator = spvtools::SpirvTools{SPV_ENV_VULKAN_1_4};
        validator.SetMessageConsumer([&](spv_message_level_t level, [[maybe_unused]] const char* source,
                                         [[maybe_unused]] const spv_position_t& position,
                                         const char* message) -> void {
            if (level <= SPV_MSG_ERROR && problem.empty()) {
                const auto text = std::string_view{message};
                problem = text.substr(0, text.find('\n'));
            }
        });

        auto rules = spvtools::ValidatorOptions{};
        rules.SetScalarBlockLayout(true);
        rules.SetWorkgroupScalarBlockLayout(true);
        rules.SetAllowLocalSizeId(true);
        rules.SetAllowOffsetTextureOperand(true);
        rules.SetAllowVulkan32BitBitwise(true);
        if (!validator.Validate(spirv.data(), spirv.size(), rules)) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_spirv,
                std::format("the module is not valid SPIR-V: {}", problem),
            });
        }

        auto state = std::make_shared<shader::state>();
        state->spirv.assign_range(spirv);

        auto reflected = SpvReflectShaderModule{};
        const auto result = spvReflectCreateShaderModule2(SPV_REFLECT_MODULE_FLAG_NO_COPY, spirv.size_bytes(),
                                                          state->spirv.data(), &reflected);
        const auto module = std::unique_ptr<SpvReflectShaderModule, decltype(&spvReflectDestroyShaderModule)>{
            &reflected, spvReflectDestroyShaderModule};
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            const auto index = static_cast<std::size_t>(result);
            return std::unexpected(rei::error{
                vk::error_code::invalid_spirv,
                std::format("SPIRV-Reflect refused the module: {}",
                            index < reflect_results.size() ? reflect_results[index] : "unknown result"),
            });
        }

        const auto entry_points = std::span{reflected.entry_points, reflected.entry_point_count};
        const auto name_of = [](const SpvReflectEntryPoint& entry) -> std::string_view {
            return entry.name != nullptr ? entry.name : "";
        };
        const auto names = entry_points | std::views::transform(name_of) |
                           std::views::join_with(std::string_view{", "}) | std::ranges::to<std::string>();

        if (entry_points.empty()) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_spirv,
                "the module has no entry point",
            });
        }
        if (entry_point.empty() && entry_points.size() > 1) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("the module has {} entry points ({}), name one", entry_points.size(), names),
            });
        }

        const auto entry = entry_point.empty() ? entry_points.begin()
                                               : std::ranges::find(entry_points, entry_point, name_of);
        if (entry == entry_points.end()) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("the module has no entry point '{}', it has {}", entry_point, names),
            });
        }

        state->stage = static_cast<vk::shader_stage>(entry->shader_stage);
        state->entry_point = name_of(*entry);
        if (state->stage != vk::shader_stage::vertex && state->stage != vk::shader_stage::fragment &&
            state->stage != vk::shader_stage::compute) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_argument,
                std::format("entry point '{}' is neither a vertex, a fragment nor a compute shader",
                            state->entry_point),
            });
        }
        if (state->stage == vk::shader_stage::compute) {
            state->local_size = {
                .width = entry->local_size.x,
                .height = entry->local_size.y,
                .depth = entry->local_size.z,
            };
        }

        const auto used_bindings = std::span{entry->used_uniforms, entry->used_uniform_count};
        for (const auto& binding :
             std::span{reflected.descriptor_bindings, reflected.descriptor_binding_count}) {
            if (!std::ranges::contains(used_bindings, binding.spirv_id)) {
                continue;
            }

            const auto type =
                static_cast<vk::descriptor_type>(std::bit_cast<std::uint32_t>(binding.descriptor_type));
            const auto name = std::string_view{binding.name != nullptr ? binding.name : ""};
            const auto block =
                binding.type_description != nullptr && binding.type_description->type_name != nullptr
                    ? std::string_view{binding.type_description->type_name}
                    : std::string_view{};
            state->bindings.push_back({
                .set = binding.set,
                .binding = binding.binding,
                .type = type,
                .count = binding.count,
                .name = std::string{name.empty() ? block : name},
            });

            const auto storage =
                type == vk::descriptor_type::storage_buffer || type == vk::descriptor_type::storage_image;
            state->writes =
                state->writes ||
                (storage && (binding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) == 0);
        }

        if (state->stage == vk::shader_stage::vertex) {
            for (const SpvReflectInterfaceVariable* variable :
                 std::span{entry->input_variables, entry->input_variable_count}) {
                const auto& input = *variable;
                if ((input.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) == 0) {
                    state->inputs.push_back({
                        .location = input.location,
                        .format = static_cast<vk::format>(std::bit_cast<std::int32_t>(input.format)),
                        .name = input.name != nullptr ? input.name : "",
                    });
                }
            }
        }

        const auto used_blocks = std::span{entry->used_push_constants, entry->used_push_constant_count};
        for (const auto& block :
             std::span{reflected.push_constant_blocks, reflected.push_constant_block_count}) {
            if (std::ranges::contains(used_blocks, block.spirv_id)) {
                state->push_constant_size = std::max(state->push_constant_size, block.size);
            }
        }

        state->writes =
            state->writes ||
            std::ranges::contains(std::span{reflected.capabilities, reflected.capability_count},
                                  SpvCapabilityPhysicalStorageBufferAddresses, &SpvReflectCapability::value);

        return vk::shader{std::move(state)};
    }

    auto shader::load(const std::filesystem::path& file, std::string_view entry_point)
        -> std::expected<vk::shader, rei::error> {
        errno = 0;
        auto source = std::ifstream{file, std::ios::binary};
        const auto bytes = std::vector<char>{std::istreambuf_iterator<char>{source}, {}};

        if (!source.is_open() || source.bad()) {
            return std::unexpected(rei::error{
                {errno != 0 ? errno : EIO, std::generic_category()},
                "failed to read " + file.string(),
            });
        }
        if (bytes.size() % sizeof(std::uint32_t) != 0) {
            return std::unexpected(rei::error{
                vk::error_code::invalid_spirv,
                std::format("{}: {} bytes isn't a whole number of SPIR-V words", file.string(), bytes.size()),
            });
        }

        auto words = std::vector<std::uint32_t>(bytes.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());

        return shader::make(words, entry_point).transform_error([&](const rei::error& error) -> rei::error {
            return rei::error{error.code(), std::format("{}: {}", file.string(), error.detail()),
                              error.location()};
        });
    }

    auto shader::stage() const noexcept -> vk::shader_stage {
        return m_state->stage;
    }

    auto shader::entry_point() const noexcept -> std::string_view {
        return m_state->entry_point;
    }

    auto shader::local_size() const noexcept -> vk::extent {
        return m_state->local_size;
    }
} // namespace rei::gpu::vk
