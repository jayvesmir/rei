#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/core/tests.hpp"
#include "rei/gpu/tests/shaders/doubling.hpp"
#include "rei/gpu/tests/shaders/fill.hpp"
#include "rei/gpu/tests/shaders/increment.hpp"
#include "rei/gpu/tests/support.hpp"
#include "rei/gpu/vk.hpp"

namespace rei::gpu::tests {
    namespace {
        constexpr auto long_name_spirv = std::to_array<std::uint32_t>({
#include "rei/gpu/tests/shaders/long_name.comp.inc"
        });

        constexpr auto deep_array_spirv = std::to_array<std::uint32_t>({
#include "rei/gpu/tests/shaders/deep_array.comp.inc"
        });

        constexpr auto spirv_that_crashed_the_reflector = std::to_array<std::uint32_t>({
            0x07230203, 0x00010600, 0x000d000b, 0x0000007d, 0x00000000, 0x00020011, 0x00000001,
            0x00060011, 0x00000032, 0x2302030b, 0x01000600, 0x00000000, 0x00110000, 0x00010002,
            0x00110000, 0x00320006, 0x030b0000, 0x06002302, 0x00070100, 0x00000106, 0x07470000,
            0x00010600, 0x47000000, 0x000b534c, 0x74732e4c,
        });

        auto storage(const vk::device& device, std::string name, float value)
            -> std::expected<vk::buffer, rei::error> {
            auto buffer = vk::buffer::make(
                device, {
                            .size = 256 * sizeof(float),
                            .usage = vk::buffer_usage::storage_buffer | vk::buffer_usage::transfer_src,
                            .host = vk::host_access::read_write,
                            .name = std::move(name),
                        });
            if (!buffer) {
                return buffer;
            }

            const auto values = std::vector<float>(256, value);
            if (const auto written = buffer->write(std::as_bytes(std::span{values})); !written) {
                return std::unexpected(written.error());
            }

            return buffer;
        }
    } // namespace

    TEST(vulkan_shaders, valid_shaders_that_overran_the_reflector_are_reflected_or_refused) {
        const auto named = vk::shader::make(long_name_spirv);
        ASSERT_TRUE(named) << named.error();
        EXPECT_EQ(named->stage(), vk::shader_stage::compute);

        EXPECT_TRUE(has_error(vk::shader::make(deep_array_spirv), vk::error_code::invalid_spirv,
                              "SPIRV-Reflect refused the module: range exceeded"));
    }

    TEST_F(vulkan, compute_passes_double_numbers_through_a_pushed_descriptor) {
        const auto queue = device().queue();
        const auto values = storage(device(), "numbers", 21.0f);
        ASSERT_TRUE(values) << values.error();

        const auto shader = shaders::doubling::compute();
        ASSERT_TRUE(shader) << shader.error();
        EXPECT_EQ(shader->stage(), vk::shader_stage::compute);
        EXPECT_EQ(shader->entry_point(), "main");
        EXPECT_EQ(shader->local_size().width, 64U);

        const auto layout = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::storage_buffer}},
                          .pushed = true,
                      });
        ASSERT_TRUE(layout) << layout.error();
        const auto pipeline = vk::compute_pipeline::make(device(), {.shader = *shader, .sets = {*layout}});
        ASSERT_TRUE(pipeline) << pipeline.error();

        auto groups = vk::buffer::make(device(), {
                                                     .size = 3 * sizeof(std::uint32_t),
                                                     .usage = vk::buffer_usage::indirect_buffer,
                                                     .host = vk::host_access::write,
                                                     .name = "groups",
                                                 });
        ASSERT_TRUE(groups) << groups.error();
        const auto counts = std::array<std::uint32_t, 3>{256 / 64, 1, 1};
        ASSERT_TRUE(groups->write(std::as_bytes(std::span{counts})));

        auto commands = queue.record("double");
        {
            auto pass = commands.compute();
            pass.bind(*pipeline);
            pass.push_descriptors(0, {{0, *values}});
            pass.dispatch(256 / 64);
        }
        {
            auto pass = commands.compute({.name = "again"});
            pass.bind(*pipeline);
            pass.push_descriptors(0, {{0, *values}});
            pass.dispatch_indirect(*groups);
        }

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        EXPECT_EQ(read<float>(*values), std::vector<float>(256, 84.0f));
        EXPECT_EQ(
            take_trace(),
            (std::vector<std::string>{
                "double: compute: barrier memory: earlier work → compute",
                "double: compute again: barrier memory: compute writes → later work; memory: earlier work → "
                "compute",
                "double: end: barrier memory: compute writes → later work",
                "submit double: ticket 1",
            }));
    }

    TEST_F(vulkan, dispatches_that_depend_on_each_other_are_ordered_unless_the_pass_is_concurrent) {
        const auto queue = device().queue();
        const auto first = storage(device(), "first", 1.0f);
        const auto second = storage(device(), "second", 0.0f);
        const auto third = storage(device(), "third", 0.0f);
        const auto before = vk::buffer::make(device(), {
                                                           .size = 256 * sizeof(float),
                                                           .usage = vk::buffer_usage::transfer_dst,
                                                           .host = vk::host_access::read_write,
                                                           .name = "before",
                                                       });
        const auto after = vk::buffer::make(device(), {
                                                          .size = 256 * sizeof(float),
                                                          .usage = vk::buffer_usage::transfer_dst,
                                                          .host = vk::host_access::read_write,
                                                          .name = "after",
                                                      });
        ASSERT_TRUE(first) << first.error();
        ASSERT_TRUE(second) << second.error();
        ASSERT_TRUE(third) << third.error();
        ASSERT_TRUE(before) << before.error();
        ASSERT_TRUE(after) << after.error();

        const auto shader = shaders::increment::compute();
        ASSERT_TRUE(shader) << shader.error();
        const auto layout = vk::set_layout::make(
            device(), {
                          .bindings =
                              {
                                  {.binding = 0, .type = vk::descriptor_type::storage_buffer},
                                  {.binding = 1, .type = vk::descriptor_type::storage_buffer},
                              },
                          .pushed = true,
                      });
        ASSERT_TRUE(layout) << layout.error();
        const auto pipeline = vk::compute_pipeline::make(device(), {.shader = *shader, .sets = {*layout}});
        ASSERT_TRUE(pipeline) << pipeline.error();

        auto chain = queue.record("chain");
        chain.copy(*third, *before);
        {
            auto pass = chain.compute();
            pass.bind(*pipeline);
            pass.push_descriptors(0, {{0, *first}, {1, *second}});
            pass.dispatch(256 / 64);
            pass.push_descriptors(0, {{0, *second}, {1, *third}});
            pass.dispatch(256 / 64);
            pass.push_descriptors(0, {{0, *third}, {1, *first}});
            pass.dispatch(256 / 64);
        }
        chain.copy(*first, *after);

        const auto chained = queue.submit(std::move(chain));
        ASSERT_TRUE(chained) << chained.error();
        ASSERT_TRUE(device().wait(*chained));

        EXPECT_EQ(read<float>(*first), std::vector<float>(256, 4.0f));
        EXPECT_EQ(read<float>(*second), std::vector<float>(256, 2.0f));
        EXPECT_EQ(read<float>(*third), std::vector<float>(256, 3.0f));
        EXPECT_EQ(read<float>(*before), std::vector<float>(256, 0.0f));
        EXPECT_EQ(read<float>(*after), std::vector<float>(256, 4.0f));
        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "chain: copy third → before: barrier before: home → transfer_dst",
                      "chain: compute: barrier before: transfer_dst → home; memory: earlier work → compute",
                      "chain: compute: barrier memory: compute writes → compute",
                      "chain: compute: barrier memory: compute writes → compute",
                      "chain: copy first → after: barrier memory: compute writes → later work; after: home → "
                      "transfer_dst",
                      "chain: end: barrier after: transfer_dst → home",
                      "submit chain: ticket 1",
                  }));

        auto fan = queue.record("fan");
        {
            auto pass = fan.compute({.concurrent = true, .name = "out"});
            pass.bind(*pipeline);
            pass.push_descriptors(0, {{0, *first}, {1, *second}});
            pass.dispatch(256 / 64);
            pass.push_descriptors(0, {{0, *first}, {1, *third}});
            pass.dispatch(256 / 64);
        }

        const auto fanned = queue.submit(std::move(fan));
        ASSERT_TRUE(fanned) << fanned.error();
        ASSERT_TRUE(device().wait(*fanned));

        EXPECT_EQ(read<float>(*second), std::vector<float>(256, 5.0f));
        EXPECT_EQ(read<float>(*third), std::vector<float>(256, 5.0f));
        EXPECT_EQ(take_trace(), (std::vector<std::string>{
                                    "fan: compute out: barrier memory: earlier work → compute",
                                    "fan: end: barrier memory: compute writes → later work",
                                    "submit fan: ticket 2",
                                }));
    }

    TEST_F(vulkan, a_storage_image_is_filled_as_its_first_use_through_a_descriptor_set) {
        const auto queue = device().queue();

        auto canvas =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 16, .height = 16},
                                          .usage = vk::image_usage::storage | vk::image_usage::transfer_src,
                                          .name = "canvas",
                                      });
        auto readback = vk::buffer::make(device(), {
                                                       .size = 16 * 16 * 4,
                                                       .usage = vk::buffer_usage::transfer_dst,
                                                       .host = vk::host_access::read_write,
                                                       .name = "readback",
                                                   });
        ASSERT_TRUE(canvas) << canvas.error();
        ASSERT_TRUE(readback) << readback.error();
        EXPECT_EQ(canvas->home(), vk::access::shader_write);

        const auto shader = shaders::fill::compute();
        ASSERT_TRUE(shader) << shader.error();
        const auto layout = vk::set_layout::make(
            device(), {.bindings = {{.binding = 0, .type = vk::descriptor_type::storage_image}}});
        ASSERT_TRUE(layout) << layout.error();
        const auto pipeline =
            vk::compute_pipeline::make(device(), {
                                                     .shader = *shader,
                                                     .sets = {*layout},
                                                     .push_constant_size = 4 * sizeof(float),
                                                 });
        ASSERT_TRUE(pipeline) << pipeline.error();

        auto target = vk::descriptor_set::make(device(), {.layout = *layout, .name = "target"});
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(target->write({{0, *canvas}}));

        auto commands = queue.record("fill");
        {
            auto pass = commands.compute({.name = "magenta"});
            pass.bind(*pipeline);
            pass.bind(0, *target);
            pass.push_constants(std::array{1.0f, 0.0f, 1.0f, 1.0f});
            pass.dispatch(16 / 8, 16 / 8);
        }
        commands.copy(*canvas, *readback);

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        auto pixels = std::array<pixel, 16 * 16>{};
        ASSERT_TRUE(readback->read(std::as_writable_bytes(std::span{pixels})));
        EXPECT_TRUE(std::ranges::all_of(
            pixels, [](const pixel& value) -> bool { return value == pixel{255, 0, 255, 255}; }));

        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "descriptor pool 1: room for 64 sets",
                      "fill: compute magenta: barrier memory: earlier work → compute",
                      "fill: copy canvas → readback: barrier memory: compute writes → later work; canvas: "
                      "shader_write → transfer_src; readback: home → transfer_dst",
                      "fill: end: barrier canvas: transfer_src → shader_write; readback: transfer_dst → home",
                      "submit fill: first use: canvas undefined → shader_write; ticket 1",
                  }));
    }

    TEST_F(vulkan,
           a_dispatch_fails_at_submit_when_a_binding_its_pipeline_reads_refers_to_a_destroyed_buffer) {
        const auto queue = device().queue();
        const auto layout = vk::set_layout::make(
            device(), {
                          .bindings =
                              {
                                  {.binding = 0, .type = vk::descriptor_type::storage_buffer},
                                  {.binding = 1, .type = vk::descriptor_type::storage_buffer},
                              },
                      });
        ASSERT_TRUE(layout) << layout.error();

        const auto doubling_shader = shaders::doubling::compute();
        const auto incrementing_shader = shaders::increment::compute();
        ASSERT_TRUE(doubling_shader) << doubling_shader.error();
        ASSERT_TRUE(incrementing_shader) << incrementing_shader.error();
        const auto doubling =
            vk::compute_pipeline::make(device(), {.shader = *doubling_shader, .sets = {*layout}});
        const auto incrementing =
            vk::compute_pipeline::make(device(), {.shader = *incrementing_shader, .sets = {*layout}});
        ASSERT_TRUE(doubling) << doubling.error();
        ASSERT_TRUE(incrementing) << incrementing.error();

        const auto kept = storage(device(), "kept", 3.0f);
        ASSERT_TRUE(kept) << kept.error();
        auto operands = vk::descriptor_set::make(device(), {.layout = *layout, .name = "operands"});
        ASSERT_TRUE(operands) << operands.error();
        {
            const auto doomed = storage(device(), "doomed", 0.0f);
            ASSERT_TRUE(doomed) << doomed.error();
            ASSERT_TRUE(operands->write({{0, *kept}, {1, *doomed}}));
        }

        const auto run = [&](const vk::compute_pipeline& pipeline) -> std::expected<void, rei::error> {
            auto commands = queue.record();
            {
                auto pass = commands.compute();
                pass.bind(pipeline);
                pass.bind(0, *operands);
                pass.dispatch(256 / 64);
            }

            return queue.submit(std::move(commands)).and_then([&](vk::ticket ticket) -> auto {
                return device().wait(ticket);
            });
        };

        ASSERT_TRUE(run(*doubling));
        EXPECT_EQ(read<float>(*kept), std::vector<float>(256, 6.0f));
        EXPECT_TRUE(
            has_error(run(*incrementing), vk::error_code::invalid_argument,
                      "dispatch in an unnamed compute pass: descriptor set 'operands' binding 1 refers "
                      "to buffer 'doomed', which was destroyed"));

        const auto replacement = storage(device(), "replacement", 0.0f);
        ASSERT_TRUE(replacement) << replacement.error();
        ASSERT_TRUE(operands->write({{1, *replacement}}));
        ASSERT_TRUE(run(*incrementing));
        EXPECT_EQ(read<float>(*replacement), std::vector<float>(256, 7.0f));
    }

    TEST_F(vulkan, a_shader_or_a_layout_that_does_not_fit_is_refused_and_the_binding_is_named) {
        EXPECT_TRUE(has_error(vk::shader::make({}), vk::error_code::invalid_spirv,
                              "the module is not valid SPIR-V: Invalid SPIR-V magic number."));
        EXPECT_TRUE(has_error(vk::shader::make(std::array<std::uint32_t, 2>{0x07230203, 0x00010600}),
                              vk::error_code::invalid_spirv,
                              "the module is not valid SPIR-V: Invalid SPIR-V header."));
        EXPECT_TRUE(has_error(vk::shader::make(std::array<std::uint32_t, 5>{1, 2, 3, 4, 5}),
                              vk::error_code::invalid_spirv,
                              "the module is not valid SPIR-V: Invalid SPIR-V magic number."));
        EXPECT_TRUE(has_error(
            vk::shader::make(std::span{shaders::doubling::spirv}.first(shaders::doubling::spirv.size() - 3)),
            vk::error_code::invalid_spirv, "the module is not valid SPIR-V: End of input reached"));
        EXPECT_TRUE(has_error(vk::shader::make(spirv_that_crashed_the_reflector),
                              vk::error_code::invalid_spirv,
                              "the module is not valid SPIR-V: Invalid instruction OpCapability"));
        EXPECT_TRUE(has_error(vk::shader::make(shaders::doubling::spirv, "other"),
                              vk::error_code::invalid_argument,
                              "the module has no entry point 'other', it has main"));

        const auto doubling = shaders::doubling::compute();
        const auto incrementing = shaders::increment::compute();
        const auto filling = shaders::fill::compute();
        ASSERT_TRUE(doubling) << doubling.error();
        ASSERT_TRUE(incrementing) << incrementing.error();
        ASSERT_TRUE(filling) << filling.error();

        const auto uniform = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::uniform_buffer}},
                          .name = "uniform",
                      });
        const auto image = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::storage_image}},
                          .pushed = true,
                          .name = "image",
                      });
        const auto single =
            vk::set_layout::make(device(), {
                                               .bindings =
                                                   {
                                                       {
                                                           .binding = 0,
                                                           .type = vk::descriptor_type::storage_buffer,
                                                           .stages = vk::shader_stage::fragment,
                                                       },
                                                   },
                                               .name = "single",
                                           });
        const auto half = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::storage_buffer}},
                          .name = "half",
                      });
        ASSERT_TRUE(uniform) << uniform.error();
        ASSERT_TRUE(image) << image.error();
        ASSERT_TRUE(single) << single.error();
        ASSERT_TRUE(half) << half.error();

        EXPECT_TRUE(has_error(vk::compute_pipeline::make(device(), {.shader = *doubling, .name = "bare"}),
                              vk::error_code::layout_mismatch,
                              "compute pipeline 'bare': the compute shader uses set 0 binding 0 ('numbers'), "
                              "and the pipeline has 0 set layouts"));
        EXPECT_TRUE(has_error(vk::compute_pipeline::make(device(), {.shader = *doubling, .sets = {*uniform}}),
                              vk::error_code::layout_mismatch,
                              "set 0 binding 0 ('numbers') is a storage_buffer in the compute shader and a "
                              "uniform_buffer in set layout 'uniform'"));
        EXPECT_TRUE(has_error(
            vk::compute_pipeline::make(device(), {.shader = *doubling, .sets = {*single}}),
            vk::error_code::layout_mismatch,
            "set 0 binding 0 ('numbers') isn't visible to the compute stage in set layout 'single'"));
        EXPECT_TRUE(has_error(
            vk::compute_pipeline::make(device(), {.shader = *incrementing, .sets = {*half}}),
            vk::error_code::layout_mismatch,
            "the compute shader uses set 0 binding 1 ('target') as a storage_buffer, and set layout 'half' "
            "has no such binding"));
        EXPECT_TRUE(has_error(vk::compute_pipeline::make(device(), {.shader = *filling, .sets = {*image}}),
                              vk::error_code::layout_mismatch,
                              "the compute shader's push constants end at byte 16, and push_constant_size "
                              "is 0"));

        EXPECT_TRUE(
            has_error(vk::set_layout::make(device(),
                                           {
                                               .bindings =
                                                   {
                                                       {.binding = 2, .type = vk::descriptor_type::sampler},
                                                       {.binding = 2, .type = vk::descriptor_type::sampler},
                                                   },
                                               .name = "twice",
                                           }),
                      vk::error_code::invalid_argument, "set layout 'twice': binding 2 is listed twice"));
        EXPECT_TRUE(has_error(vk::descriptor_set::make(device(), {.layout = *image, .name = "loose"}),
                              vk::error_code::invalid_argument,
                              "descriptor set 'loose': set layout 'image' is pushed, so it has no sets"));
    }

    TEST_F(vulkan, misuse_of_a_compute_pass_and_of_descriptors_surfaces_with_names_and_nothing_is_submitted) {
        const auto queue = device().queue();
        const auto values = storage(device(), "values", 1.0f);
        ASSERT_TRUE(values) << values.error();

        auto misplaced =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 16, .height = 16},
                                          .usage = vk::image_usage::storage | vk::image_usage::sampled,
                                          .home = vk::access::shader_read,
                                          .name = "misplaced",
                                      });
        const auto sampler = vk::sampler::make(device());
        ASSERT_TRUE(misplaced) << misplaced.error();
        ASSERT_TRUE(sampler) << sampler.error();

        const auto doubling_shader = shaders::doubling::compute();
        const auto filling_shader = shaders::fill::compute();
        ASSERT_TRUE(doubling_shader) << doubling_shader.error();
        ASSERT_TRUE(filling_shader) << filling_shader.error();

        const auto allocated = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::storage_buffer}},
                          .name = "allocated",
                      });
        const auto pushed = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::storage_image}},
                          .pushed = true,
                          .name = "pushed",
                      });
        ASSERT_TRUE(allocated) << allocated.error();
        ASSERT_TRUE(pushed) << pushed.error();

        const auto doubling = vk::compute_pipeline::make(
            device(), {.shader = *doubling_shader, .sets = {*allocated}, .name = "doubling"});
        const auto filling = vk::compute_pipeline::make(device(), {
                                                                      .shader = *filling_shader,
                                                                      .sets = {*pushed},
                                                                      .push_constant_size = 4 * sizeof(float),
                                                                      .name = "filling",
                                                                  });
        ASSERT_TRUE(doubling) << doubling.error();
        ASSERT_TRUE(filling) << filling.error();

        auto operands = vk::descriptor_set::make(device(), {.layout = *allocated, .name = "operands"});
        ASSERT_TRUE(operands) << operands.error();
        EXPECT_TRUE(has_error(
            operands->write({{0, *sampler}}), vk::error_code::invalid_argument,
            "write into descriptor set 'operands': binding 0 is a storage_buffer and was given a sampler"));
        EXPECT_TRUE(
            has_error(operands->write({{4, *values}}), vk::error_code::invalid_argument,
                      "write into descriptor set 'operands': binding 4 isn't in set layout 'allocated'"));

        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute({.name = "empty"});
                pass.dispatch(1);
            },
            "dispatch in compute pass 'empty': no pipeline is bound"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute();
                pass.bind(*doubling);
                pass.dispatch(1);
            },
            "compute pipeline 'doubling' reads set 0, and nothing is bound or pushed there"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute();
                pass.bind(*doubling);
                pass.push_descriptors(0, {{0, *values}});
            },
            "push_descriptors in an unnamed compute pass: set 0 of compute pipeline 'doubling' has a layout "
            "that isn't pushed"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute();
                pass.bind(*filling);
                pass.bind(0, *operands);
            },
            "bind of descriptor set 'operands' in an unnamed compute pass: it was made with set layout "
            "'allocated', and set 0 of compute pipeline 'filling' has set layout 'pushed'"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute();
                pass.bind(*filling);
                pass.push_descriptors(0, {{0, *misplaced}});
            },
            "binding 0 is a storage_image, and the home of image 'misplaced' is shader_read"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute();
                pass.bind(*filling);
                pass.push_constants(1.0f);
            },
            "push_constants in an unnamed compute pass: 4 bytes given, and the push_constant_size of compute "
            "pipeline 'filling' is 16"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute({.name = "open"});
                commands.copy(*values, *values);
            },
            "copy from a buffer: compute pass 'open' is still open"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.compute();
                pass.end();
                pass.dispatch(1);
            },
            "dispatch in a compute pass that has ended"));

        auto unfinished = queue.record("unfinished");
        auto pass = unfinished.compute({.name = "open"});
        EXPECT_TRUE(has_error(queue.submit(std::move(unfinished)), vk::error_code::invalid_argument,
                              "submit of unfinished: compute pass 'open' is still open"));

        ASSERT_TRUE(device().wait_idle());
        EXPECT_EQ(read<float>(*values), std::vector<float>(256, 1.0f));
    }
} // namespace rei::gpu::tests
