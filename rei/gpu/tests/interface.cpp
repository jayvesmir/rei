#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "rei/gpu/tests/shaders/fill.hpp"
#include "rei/gpu/tests/shaders/post.hpp"
#include "rei/gpu/tests/shaders/scale.hpp"
#include "rei/gpu/tests/shaders/shift.hpp"
#include "rei/gpu/tests/shaders/surface.hpp"
#include "rei/gpu/tests/support.hpp"

namespace rei::gpu::tests {
    namespace {
        struct corner {
            std::array<float, 2> position;
        };

        struct placement {
            std::array<float, 2> offset;
            std::array<float, 4> shade;
        };

        template <std::uint32_t first_location, typename... MemberTypes>
        concept surface_input =
            requires(MemberTypes... members) { shaders::surface::vertex_input<first_location>(members...); };

        static_assert(surface_input<0, decltype(&corner::position)>);
        static_assert(surface_input<1, decltype(&placement::offset), decltype(&placement::shade)>);
        static_assert(!surface_input<1, decltype(&placement::shade), decltype(&placement::offset)>);
        static_assert(!surface_input<2, decltype(&placement::offset), decltype(&placement::shade)>);

        static_assert(vk::descriptor_block<shaders::surface::set_0>);
        static_assert(!vk::descriptor_block<shaders::surface::constants>);
        static_assert(sizeof(shaders::surface::constants) == 80);
        static_assert(sizeof(shaders::post::constants) == 16);
    } // namespace

    TEST(vulkan_interfaces, generated_headers_describe_what_glsl_and_slang_shaders_declare) {
        EXPECT_EQ(shaders::post::set_0::bindings(),
                  (std::vector<vk::set_binding>{
                      {
                          .binding = 0,
                          .type = vk::descriptor_type::combined_image_sampler,
                          .stages = vk::shader_stage::fragment,
                      },
                  }));
        EXPECT_EQ(shaders::fill::local_size.width, 8U);
        EXPECT_EQ(shaders::fill::local_size.height, 8U);

        EXPECT_EQ(shaders::surface::set_0::bindings(), (std::vector<vk::set_binding>{
                                                           {
                                                               .binding = 0,
                                                               .type = vk::descriptor_type::uniform_buffer,
                                                               .stages = vk::shader_stage::fragment,
                                                           },
                                                           {
                                                               .binding = 1,
                                                               .type = vk::descriptor_type::sampled_image,
                                                               .stages = vk::shader_stage::fragment,
                                                           },
                                                           {
                                                               .binding = 2,
                                                               .type = vk::descriptor_type::sampler,
                                                               .stages = vk::shader_stage::fragment,
                                                           },
                                                       }));
        EXPECT_EQ(shaders::shift::set_0::bindings(), (std::vector<vk::set_binding>{
                                                         {
                                                             .binding = 1,
                                                             .type = vk::descriptor_type::storage_buffer,
                                                             .stages = vk::shader_stage::compute,
                                                         },
                                                     }));
        EXPECT_EQ(shaders::scale::local_size.width, 64U);
        EXPECT_EQ(shaders::shift::local_size.width, 32U);

        const auto scaling = shaders::scale::compute();
        const auto fragment = shaders::surface::fragment();
        ASSERT_TRUE(scaling) << scaling.error();
        ASSERT_TRUE(fragment) << fragment.error();
        EXPECT_EQ(scaling->entry_point(), "scale");
        EXPECT_EQ(scaling->local_size().width, 64U);
        EXPECT_EQ(fragment->stage(), vk::shader_stage::fragment);
    }

    TEST_F(vulkan, a_slang_pipeline_draws_through_its_generated_interface) {
        const auto queue = device().queue();
        const auto target = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 4, .height = 4},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .name = "target",
                      });
        const auto texture =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 1, .height = 1},
                                          .usage = vk::image_usage::sampled | vk::image_usage::transfer_dst,
                                          .name = "texture",
                                      });
        const auto sampler = vk::sampler::make(device());
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 4 * 4 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        auto colors = vk::buffer::make(device(), {
                                                     .size = 4 * sizeof(float),
                                                     .usage = vk::buffer_usage::uniform_buffer,
                                                     .host = vk::host_access::write,
                                                     .name = "colors",
                                                 });
        auto corners = vk::buffer::make(device(), {
                                                      .size = 6 * sizeof(corner),
                                                      .usage = vk::buffer_usage::vertex_buffer,
                                                      .host = vk::host_access::write,
                                                      .name = "corners",
                                                  });
        auto instances = vk::buffer::make(device(), {
                                                        .size = 2 * sizeof(placement),
                                                        .usage = vk::buffer_usage::vertex_buffer,
                                                        .host = vk::host_access::write,
                                                        .name = "instances",
                                                    });
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(texture) << texture.error();
        ASSERT_TRUE(sampler) << sampler.error();
        ASSERT_TRUE(readback) << readback.error();
        ASSERT_TRUE(colors) << colors.error();
        ASSERT_TRUE(corners) << corners.error();
        ASSERT_TRUE(instances) << instances.error();

        const auto white = std::array{1.0f, 1.0f, 1.0f, 1.0f};
        const auto square = std::array{
            corner{{-1.0f, -1.0f}}, corner{{1.0f, -1.0f}}, corner{{1.0f, 1.0f}},
            corner{{-1.0f, -1.0f}}, corner{{1.0f, 1.0f}},  corner{{-1.0f, 1.0f}},
        };
        const auto halves = std::array{
            placement{.offset = {0.0f, 0.0f}, .shade = {1.0f, 0.0f, 0.0f, 1.0f}},
            placement{.offset = {2.0f, 0.0f}, .shade = {0.0f, 1.0f, 0.0f, 1.0f}},
        };
        ASSERT_TRUE(colors->write(std::as_bytes(std::span{white})));
        ASSERT_TRUE(corners->write(std::as_bytes(std::span{square})));
        ASSERT_TRUE(instances->write(std::as_bytes(std::span{halves})));

        const auto layout = vk::set_layout::make(device(), {.bindings = shaders::surface::set_0::bindings()});
        ASSERT_TRUE(layout) << layout.error();
        auto material = vk::descriptor_set::make(device(), {.layout = *layout, .name = "material"});
        ASSERT_TRUE(material) << material.error();
        ASSERT_TRUE(material->write(shaders::surface::set_0{
            .material = *colors,
            .albedo = *texture,
            .nearest = *sampler,
        }));

        const auto vertex_shader = shaders::surface::vertex();
        const auto fragment_shader = shaders::surface::fragment();
        ASSERT_TRUE(vertex_shader) << vertex_shader.error();
        ASSERT_TRUE(fragment_shader) << fragment_shader.error();

        auto per_instance = shaders::surface::vertex_input<1>(&placement::offset, &placement::shade);
        per_instance.per_instance = true;
        const auto pipeline = vk::graphics_pipeline::make(
            device(), {
                          .vertex = *vertex_shader,
                          .fragment = *fragment_shader,
                          .vertex_input = {shaders::surface::vertex_input(&corner::position), per_instance},
                          .color = {{.format = target->format()}},
                          .sets = {*layout},
                          .push_constant_size = sizeof(shaders::surface::constants),
                          .name = "surface",
                      });
        ASSERT_TRUE(pipeline) << pipeline.error();

        const auto left_half = std::array{
            0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -0.5f, 0.0f, 0.0f, 1.0f,
        };
        const auto yellow = std::array{pixel{255, 255, 0, 255}};
        auto commands = queue.record("surface");
        commands.upload(*texture, std::as_bytes(std::span{yellow}));
        {
            auto pass = commands.render({.color = {{.target = *target}}});
            pass.bind(*pipeline);
            pass.bind(shaders::surface::set_0::set, *material);
            pass.push_constants(shaders::surface::constants{
                .projection = left_half,
                .tint = {0.0f, 0.0f, 1.0f, 1.0f},
            });
            pass.bind_vertex_buffer(0, *corners);
            pass.bind_vertex_buffer(1, *instances);
            pass.draw({.vertices = 6, .instances = 2});
        }
        commands.copy(*target, *readback);

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        const auto drawn = read<pixel>(*readback);
        EXPECT_EQ(drawn[(1 * 4) + 0], (pixel{255, 0, 255, 255}));
        EXPECT_EQ(drawn[(2 * 4) + 1], (pixel{255, 0, 255, 255}));
        EXPECT_EQ(drawn[(1 * 4) + 2], (pixel{0, 255, 255, 255}));
        EXPECT_EQ(drawn[(2 * 4) + 3], (pixel{0, 255, 255, 255}));
    }

    TEST_F(vulkan, slang_kernels_of_one_module_compute_through_their_generated_interfaces) {
        const auto queue = device().queue();
        auto source = vk::buffer::make(device(), {
                                                     .size = 256 * sizeof(float),
                                                     .usage = vk::buffer_usage::storage_buffer,
                                                     .host = vk::host_access::write,
                                                     .name = "source",
                                                 });
        const auto target = vk::buffer::make(device(), {
                                                           .size = 256 * sizeof(float),
                                                           .usage = vk::buffer_usage::storage_buffer,
                                                           .host = vk::host_access::read_write,
                                                           .name = "target",
                                                       });
        ASSERT_TRUE(source) << source.error();
        ASSERT_TRUE(target) << target.error();

        const auto twos = std::vector<float>(256, 2.0f);
        ASSERT_TRUE(source->write(std::as_bytes(std::span{twos})));

        const auto scaling_shader = shaders::scale::compute();
        const auto shifting_shader = shaders::shift::compute();
        const auto scaled = vk::set_layout::make(device(), {
                                                               .bindings = shaders::scale::set_0::bindings(),
                                                               .pushed = true,
                                                           });
        const auto shifted = vk::set_layout::make(device(), {
                                                                .bindings = shaders::shift::set_0::bindings(),
                                                                .pushed = true,
                                                            });
        ASSERT_TRUE(scaling_shader) << scaling_shader.error();
        ASSERT_TRUE(shifting_shader) << shifting_shader.error();
        ASSERT_TRUE(scaled) << scaled.error();
        ASSERT_TRUE(shifted) << shifted.error();

        const auto scaling =
            vk::compute_pipeline::make(device(), {
                                                     .shader = *scaling_shader,
                                                     .sets = {*scaled},
                                                     .push_constant_size = sizeof(shaders::scale::constants),
                                                     .name = "scaling",
                                                 });
        const auto shifting =
            vk::compute_pipeline::make(device(), {
                                                     .shader = *shifting_shader,
                                                     .sets = {*shifted},
                                                     .push_constant_size = sizeof(shaders::shift::constants),
                                                     .name = "shifting",
                                                 });
        ASSERT_TRUE(scaling) << scaling.error();
        ASSERT_TRUE(shifting) << shifting.error();

        auto commands = queue.record("kernels");
        {
            auto pass = commands.compute();
            pass.bind(*scaling);
            pass.push_descriptors(shaders::scale::set_0{.source = *source, .target = *target});
            pass.push_constants(shaders::scale::constants{.factor = 3.0f});
            pass.dispatch(256 / shaders::scale::local_size.width);

            pass.bind(*shifting);
            pass.push_descriptors(shaders::shift::set_0{.target = *target});
            pass.push_constants(shaders::shift::constants{.amount = 1.0f});
            pass.dispatch(256 / shaders::shift::local_size.width);
        }

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        EXPECT_EQ(read<float>(*target), std::vector<float>(256, 7.0f));
        EXPECT_EQ(take_trace(), (std::vector<std::string>{
                                    "kernels: compute: barrier memory: earlier work → compute",
                                    "kernels: compute: barrier memory: compute writes → compute",
                                    "kernels: end: barrier memory: compute writes → later work",
                                    "submit kernels: ticket 1",
                                }));
    }
} // namespace rei::gpu::tests
