#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "spirv_reflect/spirv_reflect.h"

namespace {
    struct entry {
        std::string_view stage;
        std::string name;
        std::size_t module{};
    };

    struct descriptor_kind {
        std::string_view type;
        std::string_view member;
        std::string_view element;

        auto operator==(const descriptor_kind& other) const noexcept -> bool = default;
    };

    struct binding {
        descriptor_kind kind;
        std::uint32_t count{};
        std::string name;
        std::vector<std::string_view> stages;
    };

    struct constant {
        std::string name;
        std::string type;
        std::uint32_t offset{};
        std::uint32_t size{};
        std::uint32_t alignment{};

        auto operator==(const constant& other) const noexcept -> bool = default;
    };

    struct pipeline_shaders {
        std::vector<std::vector<std::uint32_t>> modules;
        std::vector<entry> entries;
        std::map<std::uint32_t, std::map<std::uint32_t, binding>> sets;
        std::optional<std::vector<constant>> constants;
        std::vector<std::string> vertex_formats;
        std::array<std::uint32_t, 3> local_size{};
    };

    auto member_name(std::string_view reflected, std::string_view what)
        -> std::expected<std::string, std::string> {
        const auto dot = reflected.rfind('.');
        const auto name = dot == std::string_view::npos ? reflected : reflected.substr(dot + 1);

        const auto letter = [](char symbol) -> bool {
            return symbol == '_' || (symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z');
        };
        if (name.empty() || !letter(name.front()) || !std::ranges::all_of(name, [&](char symbol) -> bool {
                return letter(symbol) || (symbol >= '0' && symbol <= '9');
            })) {
            return std::unexpected(std::format("{} is named '{}' in the SPIR-V, which can't name a C++ "
                                               "member; compile the shader with its names",
                                               what, reflected));
        }

        return std::string{name};
    }

    auto mirror(const SpvReflectBlockVariable& member) -> std::expected<constant, std::string> {
        const auto& type = *member.type_description;
        const auto& numeric = member.numeric;
        const auto reflected = std::string_view{member.name != nullptr ? member.name : ""};

        auto name = member_name(reflected, "a push constant");
        if (!name) {
            return std::unexpected(std::move(name).error());
        }
        if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT) != 0 && member.member_count == 1 &&
            member.members[0].offset == 0) {
            return mirror(member.members[0]).transform([&](constant wrapped) -> constant {
                wrapped.name = std::move(*name);
                wrapped.offset = member.offset;

                return wrapped;
            });
        }
        if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_STRUCT) != 0) {
            return std::unexpected(std::format("push constant '{}' is a struct; only scalars, vectors, "
                                               "matrices and arrays of them are mirrored",
                                               reflected));
        }

        const auto width = numeric.scalar.width / 8;
        auto scalar = std::string{};
        if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0 && (width == 4 || width == 8)) {
            scalar = width == 4 ? "float" : "double";
        } else if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_INT) != 0 && std::has_single_bit(width) &&
                   width <= 8) {
            scalar = std::format("std::{}int{}_t", numeric.scalar.signedness != 0 ? "" : "u", width * 8);
        } else if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_BOOL) != 0) {
            scalar = "std::uint32_t";
        } else {
            return std::unexpected(
                std::format("push constant '{}' has a type with no C++ mirror", reflected));
        }

        auto mirrored = constant{
            .name = std::move(*name),
            .type = scalar,
            .offset = member.offset,
            .size = width,
            .alignment = width,
        };
        if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_MATRIX) != 0) {
            const auto row_major = (member.decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0;
            const auto vectors = row_major ? numeric.matrix.row_count : numeric.matrix.column_count;
            const auto components = row_major ? numeric.matrix.column_count : numeric.matrix.row_count;

            mirrored.size = vectors * numeric.matrix.stride;
            mirrored.type = numeric.matrix.stride == components * width
                                ? std::format("std::array<{}, {}>", scalar, vectors * components)
                                : std::format("std::array<std::array<{}, {}>, {}>", scalar,
                                              numeric.matrix.stride / width, vectors);
        } else if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_VECTOR) != 0) {
            mirrored.size = numeric.vector.component_count * width;
            mirrored.type = std::format("std::array<{}, {}>", scalar, numeric.vector.component_count);
        }

        if ((type.type_flags & SPV_REFLECT_TYPE_FLAG_ARRAY) != 0) {
            if (member.array.dims_count != 1 || member.array.dims[0] == 0 ||
                member.array.stride != mirrored.size) {
                return std::unexpected(
                    std::format("push constant '{}' is an array with {} dimensions and {} bytes per element "
                                "of {}; only arrays "
                                "of one dimension without padding between elements are mirrored",
                                reflected, member.array.dims_count, member.array.stride, mirrored.size));
            }

            mirrored.type = std::format("std::array<{}, {}>", mirrored.type, member.array.dims[0]);
            mirrored.size *= member.array.dims[0];
        }

        return mirrored;
    }

    auto read(pipeline_shaders& program, const std::string& file, std::span<const std::string_view> wanted)
        -> std::expected<void, std::string> {
        auto source = std::ifstream{file, std::ios::binary};
        const auto bytes = std::vector<char>{std::istreambuf_iterator<char>{source}, {}};
        if (!source.is_open() || source.bad() || bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
            return std::unexpected(std::format("{}: can't read a whole number of SPIR-V words", file));
        }

        auto& words = program.modules.emplace_back(bytes.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());

        auto reflected = SpvReflectShaderModule{};
        const auto result = spvReflectCreateShaderModule2(SPV_REFLECT_MODULE_FLAG_NO_COPY, bytes.size(),
                                                          words.data(), &reflected);
        const auto module = std::unique_ptr<SpvReflectShaderModule, decltype(&spvReflectDestroyShaderModule)>{
            &reflected, spvReflectDestroyShaderModule};
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            return std::unexpected(std::format("{}: SPIRV-Reflect refused the module (result {})", file,
                                               std::to_underlying(result)));
        }

        const auto before = program.entries.size();
        for (const auto& point : std::span{reflected.entry_points, reflected.entry_point_count}) {
            const auto point_name = std::string_view{point.name != nullptr ? point.name : ""};
            if (!wanted.empty() && !std::ranges::contains(wanted, point_name)) {
                continue;
            }

            auto stage = std::string_view{};
            if (point.shader_stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
                stage = "vertex";
            } else if (point.shader_stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT) {
                stage = "fragment";
            } else if (point.shader_stage == SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) {
                stage = "compute";
            }
            if (stage.empty()) {
                return std::unexpected(
                    std::format("{}: entry point '{}' is neither a vertex, a fragment nor a compute shader",
                                file, point_name));
            }
            if (const auto taken = std::ranges::find(program.entries, stage, &entry::stage);
                taken != program.entries.end()) {
                return std::unexpected(std::format("{}: entry point '{}' is a second {} shader beside '{}'; "
                                                   "one target is one pipeline's shaders, "
                                                   "so name the one to use in entry_points",
                                                   file, point_name, stage, taken->name));
            }

            program.entries.push_back({
                .stage = stage,
                .name = std::string{point_name},
                .module = program.modules.size() - 1,
            });

            const auto used_bindings = std::span{point.used_uniforms, point.used_uniform_count};
            for (const auto& reflected_binding :
                 std::span{reflected.descriptor_bindings, reflected.descriptor_binding_count}) {
                if (!std::ranges::contains(used_bindings, reflected_binding.spirv_id)) {
                    continue;
                }

                const auto where = std::format("{}: set {} binding {}", file, reflected_binding.set,
                                               reflected_binding.binding);
                auto kind = descriptor_kind{};
                switch (reflected_binding.descriptor_type) {
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
                    kind = {
                        .type = "sampler",
                        .member = "const vk::sampler&",
                        .element = "std::reference_wrapper<const vk::sampler>",
                    };
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    kind = {
                        .type = "combined_image_sampler",
                        .member = "vk::image_sampler",
                        .element = "vk::image_sampler",
                    };
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    kind = {
                        .type = "sampled_image",
                        .member = "const vk::image_view&",
                        .element = "std::reference_wrapper<const vk::image_view>",
                    };
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    kind = {
                        .type = "storage_image",
                        .member = "const vk::image_view&",
                        .element = "std::reference_wrapper<const vk::image_view>",
                    };
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    kind = {
                        .type = "uniform_buffer",
                        .member = "vk::buffer_range",
                        .element = "vk::buffer_range",
                    };
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    kind = {
                        .type = "storage_buffer",
                        .member = "vk::buffer_range",
                        .element = "vk::buffer_range",
                    };
                    break;
                default:
                    break;
                }
                if (kind.type.empty()) {
                    return std::unexpected(
                        std::format("{} has a descriptor type that rei::gpu::vk doesn't have", where));
                }
                if (reflected_binding.count == 0) {
                    return std::unexpected(std::format(
                        "{} is an array without a size, which a generated struct can't hold", where));
                }

                const auto variable =
                    std::string_view{reflected_binding.name != nullptr ? reflected_binding.name : ""};
                const auto block = reflected_binding.type_description != nullptr &&
                                           reflected_binding.type_description->type_name != nullptr
                                       ? std::string_view{reflected_binding.type_description->type_name}
                                       : std::string_view{};
                auto name = member_name(variable.empty() ? block : variable, where);
                if (!name) {
                    return std::unexpected(std::move(name).error());
                }

                if (*name == "set" || *name == "bindings" || *name == "descriptors") {
                    return std::unexpected(std::format(
                        "{} is named '{}', which the generated struct uses itself; rename it in the shader",
                        where, *name));
                }

                auto& merged = program.sets[reflected_binding.set][reflected_binding.binding];
                if (merged.stages.empty()) {
                    merged = {.kind = kind, .count = reflected_binding.count, .name = *name, .stages = {}};
                }
                if (merged.kind != kind || merged.count != reflected_binding.count || merged.name != *name) {
                    return std::unexpected(
                        std::format("{} is {} '{}' in the {} shader and {} '{}' in an earlier one", where,
                                    kind.type, *name, stage, merged.kind.type, merged.name));
                }

                merged.stages.push_back(stage);
            }

            const auto used_blocks = std::span{point.used_push_constants, point.used_push_constant_count};
            for (const auto& block :
                 std::span{reflected.push_constant_blocks, reflected.push_constant_block_count}) {
                if (!std::ranges::contains(used_blocks, block.spirv_id)) {
                    continue;
                }

                auto constants = std::vector<constant>{};
                for (const auto& member : std::span{block.members, block.member_count}) {
                    auto mirrored = mirror(member);
                    if (!mirrored) {
                        return std::unexpected(std::format("{}: {}", file, mirrored.error()));
                    }

                    constants.push_back(std::move(*mirrored));
                }

                std::ranges::sort(constants, {}, &constant::offset);
                if (program.constants && *program.constants != constants) {
                    return std::unexpected(std::format("{}: the {} shader's push constants differ from an "
                                                       "earlier shader's. A pipeline has one "
                                                       "push-constant block that every stage reads from byte "
                                                       "0, so declare one block and use it in "
                                                       "both (in Slang, a global [[vk::push_constant]] "
                                                       "instead of uniform entry-point parameters)",
                                                       file, stage));
                }

                program.constants = std::move(constants);
            }

            if (point.shader_stage == SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) {
                program.local_size = {point.local_size.x, point.local_size.y, point.local_size.z};
            }
            if (point.shader_stage != SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
                continue;
            }

            for (const SpvReflectInterfaceVariable* variable :
                 std::span{point.input_variables, point.input_variable_count}) {
                const auto& input = *variable;
                if ((input.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                    continue;
                }

                auto kind = std::string_view{"sfloat"};
                if ((input.type_description->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) == 0) {
                    kind = input.numeric.scalar.signedness != 0 ? "sint" : "uint";
                }

                if (input.format == SPV_REFLECT_FORMAT_UNDEFINED) {
                    return std::unexpected(
                        std::format("{}: vertex input '{}' at location {} has no vertex format", file,
                                    input.name != nullptr ? input.name : "", input.location));
                }

                auto format = std::string{};
                for (const auto channel :
                     std::string_view{"rgba"}.substr(0, std::max(input.numeric.vector.component_count, 1U))) {
                    format += std::format("{}{}", channel, input.numeric.scalar.width);
                }

                program.vertex_formats.resize(
                    std::max<std::size_t>(program.vertex_formats.size(), input.location + 1), "undefined");
                program.vertex_formats[input.location] = std::format("{}_{}", format, kind);
            }
        }

        if (program.entries.size() == before) {
            return std::unexpected(std::format("{}: none of its entry points was asked for", file));
        }

        return {};
    }

    auto write(const pipeline_shaders& program, std::string_view name_space) -> std::string {
        auto header = std::string{};
        const auto line = [&]<typename... ValueTypes>(std::format_string<ValueTypes...> text,
                                                      ValueTypes&&... values) -> void {
            std::format_to(std::back_inserter(header), text, std::forward<ValueTypes>(values)...);
            header += '\n';
        };

        line("#pragma once\n");
        for (const std::string_view include :
             {"algorithm", "array", "cstddef", "cstdint", "expected", "functional", "span", "vector"}) {
            line("#include <{}>", include);
        }
        line("\n#include \"rei/core/error.hpp\"\n#include \"rei/gpu/vk/pipeline.hpp\"\n");
        line("namespace {} {{", name_space);
        line("    namespace vk = rei::gpu::vk;");

        const auto spirv_name = [&](std::size_t module) -> std::string {
            const auto first = std::ranges::find(program.entries, module, &entry::module);
            return program.modules.size() == 1 ? "spirv" : std::format("{}_spirv", first->stage);
        };
        for (const auto& [index, words] : std::views::zip(std::views::iota(0uz), program.modules)) {
            line("\n    inline constexpr auto {} = std::array<std::uint32_t, {}>{{", spirv_name(index),
                 words.size());
            for (auto i = 0uz; i < words.size(); i++) {
                std::format_to(std::back_inserter(header), "{}{:#010x},{}", i % 8 == 0 ? "        " : " ",
                               words[i], i % 8 == 7 || i + 1 == words.size() ? "\n" : "");
            }
            line("    }};");
        }

        for (const auto& point : program.entries) {
            line("\n    [[nodiscard]] inline auto {}() -> std::expected<vk::shader, rei::error> {{",
                 point.stage);
            line("        return vk::shader::make({}, \"{}\");", spirv_name(point.module), point.name);
            line("    }}");
        }

        if (std::ranges::contains(program.entries, std::string_view{"compute"}, &entry::stage)) {
            line("\n    inline constexpr auto local_size = vk::extent{{.width = {}, .height = {}, .depth = "
                 "{}}};",
                 program.local_size[0], program.local_size[1], program.local_size[2]);
        }

        if (!program.vertex_formats.empty()) {
            line("\n    inline constexpr auto vertex_formats = std::array{{");
            for (const auto& format : program.vertex_formats) {
                line("        vk::format::{},", format);
            }
            line("    }};\n");
            line("    template <std::uint32_t first_location = 0, typename VertexType, typename... "
                 "MemberTypes>");
            line("        requires(first_location + sizeof...(MemberTypes) <= vertex_formats.size() &&");
            line("                 std::ranges::equal(std::array{{vk::vertex_format<MemberTypes>...}},");
            line("                                    std::span{{vertex_formats}}.subspan(first_location,");
            line("                                                                      "
                 "sizeof...(MemberTypes))))");
            line("    auto vertex_input(MemberTypes VertexType::*... members) -> vk::vertex_binding {{");
            line("        auto binding = vk::vertex_layout(members...);");
            line("        for (auto& attribute : binding.attributes) {{");
            line("            attribute.location += first_location;");
            line("        }}\n");
            line("        return binding;");
            line("    }}");
        }

        if (program.constants) {
            auto end = std::uint32_t{};
            auto alignment = std::uint32_t{1};
            auto paddings = std::uint32_t{};
            const auto pad = [&](std::uint32_t until) -> void {
                if (until > end) {
                    line("        std::array<std::byte, {}> padding_{}{{}};", until - end, paddings++);
                    end = until;
                }
            };

            line("\n    struct constants {{");
            for (const auto& member : *program.constants) {
                pad(member.offset);
                line("        {} {};", member.type, member.name);
                end = member.offset + member.size;
                alignment = std::max(alignment, member.alignment);
            }
            pad((end + 3) / 4 * 4);
            line("    }};\n");

            line("    static_assert(sizeof(constants) == {});",
                 (end + alignment - 1) / alignment * alignment);
            for (const auto& member : *program.constants) {
                line("    static_assert(offsetof(constants, {}) == {});", member.name, member.offset);
            }
        }

        for (const auto& [set, bindings] : program.sets) {
            auto descriptors = std::vector<std::string>{};

            line("\n    struct set_{} {{", set);
            line("        static constexpr auto set = std::uint32_t{{{}}};\n", set);
            for (const auto& [number, bound] : bindings) {
                if (bound.count == 1) {
                    line("        {} {};", bound.kind.member, bound.name);
                    descriptors.push_back(std::format("vk::descriptor{{{}, {}}}", number, bound.name));
                    continue;
                }

                line("        std::array<{}, {}> {};", bound.kind.element, bound.count, bound.name);
                for (auto i = 0U; i < bound.count; i++) {
                    descriptors.push_back(
                        std::format("vk::descriptor{{{}, {}[{}]}}.at({})", number, bound.name, i, i));
                }
            }

            line("\n        [[nodiscard]] static auto bindings() -> std::vector<vk::set_binding> {{");
            line("            return {{");
            for (const auto& [number, bound] : bindings) {
                line("                {{");
                line("                    .binding = {},", number);
                line("                    .type = vk::descriptor_type::{},", bound.kind.type);
                line("                    .stages = {},{}",
                     bound.stages | std::views::transform([](std::string_view stage) -> std::string {
                         return std::format("vk::shader_stage::{}", stage);
                     }) | std::views::join_with(std::string_view{" | "}) |
                         std::ranges::to<std::string>(),
                     bound.count == 1 ? "" : std::format("\n                    .count = {},", bound.count));
                line("                }},");
            }
            line("            }};");
            line("        }}\n");
            line("        [[nodiscard]] auto descriptors() const -> std::array<vk::descriptor, {}> {{",
                 descriptors.size());
            line("            return {{");
            for (const auto& descriptor : descriptors) {
                line("                {},", descriptor);
            }
            line("            }};");
            line("        }}");
            line("    }};");
        }

        line("}} // namespace {}", name_space);

        return header;
    }
} // namespace

auto main(int argc, char** argv) -> int {
    const auto arguments = std::span{argv, static_cast<std::size_t>(argc)}.subspan(1) |
                           std::ranges::to<std::vector<std::string_view>>();

    auto name_space = std::string_view{};
    auto output = std::string{};
    auto wanted = std::vector<std::string_view>{};
    auto files = std::vector<std::string>{};
    for (auto i = 0uz; i < arguments.size(); i++) {
        const auto option =
            arguments[i] == "--namespace" || arguments[i] == "--output" || arguments[i] == "--entry";
        if (!option) {
            files.emplace_back(arguments[i]);
            continue;
        }
        if (i + 1 == arguments.size()) {
            std::println(stderr, "error: {} needs a value", arguments[i]);
            return 1;
        }

        i++;
        if (arguments[i - 1] == "--namespace") {
            name_space = arguments[i];
        } else if (arguments[i - 1] == "--output") {
            output = arguments[i];
        } else {
            wanted.push_back(arguments[i]);
        }
    }

    if (name_space.empty() || output.empty() || files.empty()) {
        std::println(stderr,
                     "usage: interface --namespace NAME --output HEADER [--entry NAME]... MODULE.spv...");
        return 1;
    }

    auto program = pipeline_shaders{};
    for (const auto& file : files) {
        if (const auto done = read(program, file, wanted); !done) {
            std::println(stderr, "error: {}", done.error());
            return 1;
        }
    }

    for (const auto name : wanted) {
        if (!std::ranges::contains(program.entries, name, &entry::name)) {
            std::println(stderr, "error: no module has an entry point '{}'", name);
            return 1;
        }
    }

    auto header = std::ofstream{output, std::ios::binary};
    header << write(program, name_space);
    header.close();
    if (header.fail()) {
        std::println(stderr, "error: can't write {}", output);
        return 1;
    }

    return 0;
}
