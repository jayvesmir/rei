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
#include "rei/gpu/tests/shaders/mark.hpp"
#include "rei/gpu/tests/shaders/post.hpp"
#include "rei/gpu/tests/shaders/scene.hpp"
#include "rei/gpu/tests/shaders/triangle.hpp"
#include "rei/gpu/tests/support.hpp"
#include "rei/gpu/vk.hpp"

namespace rei::gpu::tests {
    namespace {
        struct vertex {
            std::array<float, 2> position;
            std::array<float, 3> color;
        };

        struct placement {
            std::array<float, 3> place;
            std::array<float, 3> color;
        };

        auto green_triangle(const vk::device& device) -> std::expected<vk::buffer, rei::error> {
            auto buffer = vk::buffer::make(device, {
                                                       .size = 3 * sizeof(vertex),
                                                       .usage = vk::buffer_usage::vertex_buffer,
                                                       .host = vk::host_access::write,
                                                       .name = "triangle",
                                                   });
            if (!buffer) {
                return buffer;
            }

            const auto corners = std::array{
                vertex{.position = {-1.0f, -1.0f}, .color = {0.0f, 1.0f, 0.0f}},
                vertex{.position = {1.0f, -1.0f}, .color = {0.0f, 1.0f, 0.0f}},
                vertex{.position = {-1.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f}},
            };
            if (const auto written = buffer->write(std::as_bytes(std::span{corners})); !written) {
                return std::unexpected(written.error());
            }

            return buffer;
        }
    } // namespace

    TEST_F(vulkan, a_triangle_is_drawn_into_an_offscreen_target_and_read_back) {
        const auto queue = device().queue();
        const auto target = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .name = "target",
                      });
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 16 * 16 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        const auto vertices = green_triangle(device());
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(readback) << readback.error();
        ASSERT_TRUE(vertices) << vertices.error();
        EXPECT_EQ(target->home(), vk::access::color_attachment);

        const auto layout = vk::vertex_layout(&vertex::position, &vertex::color);
        ASSERT_EQ(layout.attributes.size(), 2U);
        EXPECT_EQ(layout.stride, sizeof(vertex));
        EXPECT_EQ(layout.attributes[0].format, vk::format::r32g32_sfloat);
        EXPECT_EQ(layout.attributes[1].location, 1U);
        EXPECT_EQ(layout.attributes[1].format, vk::format::r32g32b32_sfloat);
        EXPECT_EQ(layout.attributes[1].offset, offsetof(vertex, color));

        const auto vertex_shader = shaders::triangle::vertex();
        const auto fragment_shader = shaders::triangle::fragment();
        ASSERT_TRUE(vertex_shader) << vertex_shader.error();
        ASSERT_TRUE(fragment_shader) << fragment_shader.error();
        EXPECT_EQ(vertex_shader->stage(), vk::shader_stage::vertex);
        const auto pipeline =
            vk::graphics_pipeline::make(device(), {
                                                      .vertex = *vertex_shader,
                                                      .fragment = *fragment_shader,
                                                      .vertex_input = {layout},
                                                      .color = {{.format = target->format()}},
                                                      .name = "triangle",
                                                  });
        ASSERT_TRUE(pipeline) << pipeline.error();

        auto commands = queue.record("triangle");
        {
            auto pass = commands.render({.color = {{.target = *target, .clear = {0.0f, 0.0f, 1.0f, 1.0f}}}});
            pass.bind(*pipeline);
            pass.bind_vertex_buffer(0, *vertices);
            pass.draw({.vertices = 3});
        }
        commands.copy(*target, *readback);

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        const auto drawn = read<pixel>(*readback);
        EXPECT_EQ(drawn[(2 * 16) + 2], (pixel{0, 255, 0, 255}));
        EXPECT_EQ(drawn[(13 * 16) + 13], (pixel{0, 0, 255, 255}));
        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "triangle: copy target → readback: barrier target: color_attachment → transfer_src "
                      "(folded); readback: home → transfer_dst",
                      "triangle: end: barrier target: transfer_src → color_attachment; readback: "
                      "transfer_dst → home",
                      "submit triangle: first use: target undefined → color_attachment; ticket 1",
                  }));
    }

    TEST_F(vulkan, a_pass_over_part_of_a_target_keeps_what_is_outside_its_area) {
        const auto queue = device().queue();
        const auto target =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 16, .height = 16},
                                          .usage = vk::image_usage::color_attachment |
                                                   vk::image_usage::sampled | vk::image_usage::transfer_src,
                                          .name = "target",
                                      });
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 16 * 16 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(readback) << readback.error();
        EXPECT_EQ(target->home(), vk::access::shader_read);

        auto commands = queue.record("halves");
        {
            auto pass = commands.render({.color = {{.target = *target, .clear = {0.0f, 0.0f, 1.0f, 1.0f}}}});
        }
        {
            auto pass = commands.render({
                .color = {{.target = *target, .clear = {1.0f, 0.0f, 0.0f, 1.0f}}},
                .area = vk::area{.x = 8, .width = 8, .height = 16},
            });
        }
        commands.copy(*target, *readback);

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        const auto drawn = read<pixel>(*readback);
        EXPECT_EQ(drawn[0], (pixel{0, 0, 255, 255}));
        EXPECT_EQ(drawn[15], (pixel{255, 0, 0, 255}));
        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "halves: render: barrier target: shader_read → color_attachment, discard",
                      "halves: render: barrier target: color_attachment → color_attachment (folded)",
                      "halves: copy target → readback: barrier target: color_attachment → transfer_src "
                      "(folded); readback: home → transfer_dst",
                      "halves: end: barrier target: transfer_src → shader_read; readback: transfer_dst → "
                      "home",
                      "submit halves: first use: target undefined → shader_read; ticket 1",
                  }));
    }

    TEST_F(vulkan, a_depth_target_with_stencil_is_sampled_by_a_later_pass_through_its_depth) {
        const auto queue = device().queue();
        const auto depth = vk::image::make(
            device(), {
                          .format = vk::format::d32_sfloat_s8_uint,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::depth_stencil_attachment | vk::image_usage::sampled,
                          .name = "depth",
                      });
        const auto output = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .name = "output",
                      });
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 16 * 16 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        const auto sampler = vk::sampler::make(device(), {
                                                             .mag = vk::filter::nearest,
                                                             .min = vk::filter::nearest,
                                                         });
        ASSERT_TRUE(depth) << depth.error();
        ASSERT_TRUE(output) << output.error();
        ASSERT_TRUE(readback) << readback.error();
        ASSERT_TRUE(sampler) << sampler.error();

        const auto layout = vk::set_layout::make(
            device(), {.bindings = {{.binding = 0, .type = vk::descriptor_type::combined_image_sampler}}});
        ASSERT_TRUE(layout) << layout.error();
        auto sampled = vk::descriptor_set::make(device(), {.layout = *layout, .name = "sampled"});
        ASSERT_TRUE(sampled) << sampled.error();
        ASSERT_TRUE(sampled->write({{0, *depth, *sampler}}));

        const auto post_vertex = shaders::post::vertex();
        const auto post_fragment = shaders::post::fragment();
        ASSERT_TRUE(post_vertex) << post_vertex.error();
        ASSERT_TRUE(post_fragment) << post_fragment.error();
        const auto post =
            vk::graphics_pipeline::make(device(), {
                                                      .vertex = *post_vertex,
                                                      .fragment = *post_fragment,
                                                      .color = {{.format = vk::format::r8g8b8a8_unorm}},
                                                      .sets = {*layout},
                                                      .push_constant_size = 4 * sizeof(float),
                                                      .name = "post",
                                                  });
        ASSERT_TRUE(post) << post.error();

        auto commands = queue.record("frame");
        commands.render({.depth = vk::depth_target{.target = *depth, .clear = 0.5f}, .name = "clear"}).end();
        {
            auto pass = commands.render({.color = {{.target = *output}}, .name = "post"});
            pass.bind(*post);
            pass.bind(0, *sampled);
            pass.push_constants(std::array{1.0f, 1.0f, 1.0f, 1.0f});
            pass.draw({.vertices = 3});
        }
        commands.copy(*output, *readback);

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        const auto drawn = read<pixel>(*readback);
        EXPECT_NEAR(drawn[(8 * 16) + 8][0], 128, 1);
    }

    TEST_F(vulkan, a_multisampled_scene_with_depth_is_resolved_into_a_target_that_a_second_pass_samples) {
        const auto queue = device().queue();
        const auto multisampled = vk::image::make(device(), {
                                                                .format = vk::format::r8g8b8a8_unorm,
                                                                .extent = {.width = 16, .height = 16},
                                                                .usage = vk::image_usage::color_attachment,
                                                                .samples = 4,
                                                                .name = "multisampled",
                                                            });
        const auto depth = vk::image::make(device(), {
                                                         .format = vk::format::d32_sfloat,
                                                         .extent = {.width = 16, .height = 16},
                                                         .usage = vk::image_usage::depth_stencil_attachment,
                                                         .samples = 4,
                                                         .name = "depth",
                                                     });
        const auto scene = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::sampled,
                          .name = "scene",
                      });
        const auto output = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .name = "output",
                      });
        const auto readback = vk::buffer::make(device(), {
                                                             .size = 16 * 16 * sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "readback",
                                                         });
        const auto sampler = vk::sampler::make(device(), {
                                                             .mag = vk::filter::nearest,
                                                             .min = vk::filter::nearest,
                                                         });
        ASSERT_TRUE(multisampled) << multisampled.error();
        ASSERT_TRUE(depth) << depth.error();
        ASSERT_TRUE(scene) << scene.error();
        ASSERT_TRUE(output) << output.error();
        ASSERT_TRUE(readback) << readback.error();
        ASSERT_TRUE(sampler) << sampler.error();
        EXPECT_EQ(scene->home(), vk::access::shader_read);

        auto corners = vk::buffer::make(device(), {
                                                      .size = 4 * 2 * sizeof(float),
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
        auto indices = vk::buffer::make(device(), {
                                                      .size = 6 * sizeof(std::uint16_t),
                                                      .usage = vk::buffer_usage::index_buffer,
                                                      .host = vk::host_access::write,
                                                      .name = "indices",
                                                  });
        ASSERT_TRUE(corners) << corners.error();
        ASSERT_TRUE(instances) << instances.error();
        ASSERT_TRUE(indices) << indices.error();

        const auto quad = std::array{-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
        const auto placed = std::array{
            placement{.place = {-0.25f, -0.25f, 0.25f}, .color = {0.0f, 1.0f, 0.0f}},
            placement{.place = {0.25f, 0.25f, 0.75f}, .color = {1.0f, 0.0f, 0.0f}},
        };
        const auto order = std::array<std::uint16_t, 6>{0, 1, 2, 2, 3, 0};
        ASSERT_TRUE(corners->write(std::as_bytes(std::span{quad})));
        ASSERT_TRUE(instances->write(std::as_bytes(std::span{placed})));
        ASSERT_TRUE(indices->write(std::as_bytes(std::span{order})));

        auto per_instance = vk::vertex_layout(&placement::place, &placement::color);
        per_instance.per_instance = true;
        per_instance.attributes[0].location = 1;
        per_instance.attributes[1].location = 2;

        const auto scene_vertex = shaders::scene::vertex();
        const auto flat_fragment = shaders::triangle::fragment();
        const auto post_vertex = shaders::post::vertex();
        const auto post_fragment = shaders::post::fragment();
        ASSERT_TRUE(scene_vertex) << scene_vertex.error();
        ASSERT_TRUE(flat_fragment) << flat_fragment.error();
        ASSERT_TRUE(post_vertex) << post_vertex.error();
        ASSERT_TRUE(post_fragment) << post_fragment.error();

        const auto layout = vk::set_layout::make(
            device(), {.bindings = {{.binding = 0, .type = vk::descriptor_type::combined_image_sampler}}});
        ASSERT_TRUE(layout) << layout.error();
        auto sampled = vk::descriptor_set::make(device(), {.layout = *layout, .name = "sampled"});
        ASSERT_TRUE(sampled) << sampled.error();
        ASSERT_TRUE(sampled->write({{0, *scene, *sampler}}));

        const auto quads = vk::graphics_pipeline::make(
            device(), {
                          .vertex = *scene_vertex,
                          .fragment = *flat_fragment,
                          .vertex_input =
                              {
                                  {
                                      .stride = 2 * sizeof(float),
                                      .attributes =
                                          {
                                              {
                                                  .location = 0,
                                                  .format = vk::format::r32g32_sfloat,
                                                  .offset = 0,
                                              },
                                          },
                                  },
                                  per_instance,
                              },
                          .color = {{.format = vk::format::r8g8b8a8_unorm}},
                          .depth = vk::depth_output{.format = vk::format::d32_sfloat},
                          .push_constant_size = sizeof(float),
                          .samples = 4,
                          .name = "quads",
                      });
        const auto post =
            vk::graphics_pipeline::make(device(), {
                                                      .vertex = *post_vertex,
                                                      .fragment = *post_fragment,
                                                      .color = {{.format = vk::format::r8g8b8a8_unorm}},
                                                      .sets = {*layout},
                                                      .push_constant_size = 4 * sizeof(float),
                                                      .name = "post",
                                                  });
        ASSERT_TRUE(quads) << quads.error();
        ASSERT_TRUE(post) << post.error();

        auto commands = queue.record("frame");
        {
            auto pass = commands.render({
                .color =
                    {
                        {
                            .target = *multisampled,
                            .store = vk::store_op::dont_care,
                            .clear = {0.0f, 0.0f, 1.0f, 1.0f},
                            .resolve = vk::resolve_target{*scene},
                        },
                    },
                .depth = vk::depth_target{.target = *depth, .store = vk::store_op::dont_care},
                .name = "scene",
            });
            pass.bind(*quads);
            pass.push_constants(0.5f);
            pass.bind_vertex_buffer(0, *corners);
            pass.bind_vertex_buffer(1, *instances);
            pass.bind_index_buffer(*indices, vk::index_type::uint16);
            pass.draw_indexed({.indices = 6, .instances = 2});
        }
        {
            auto pass = commands.render({.color = {{.target = *output}}, .name = "post"});
            pass.bind(*post);
            pass.bind(0, *sampled);
            pass.push_constants(std::array{1.0f, 1.0f, 0.0f, 1.0f});
            pass.draw({.vertices = 3});
        }
        commands.copy(*output, *readback);

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        const auto drawn = read<pixel>(*readback);
        EXPECT_EQ(drawn[0], (pixel{0, 0, 0, 255}));
        EXPECT_EQ(drawn[(3 * 16) + 3], (pixel{0, 255, 0, 255}));
        EXPECT_EQ(drawn[(8 * 16) + 8], (pixel{0, 255, 0, 255}));
        EXPECT_EQ(drawn[(12 * 16) + 12], (pixel{255, 0, 0, 255}));
        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "descriptor pool 1: room for 64 sets",
                      "frame: render scene: barrier scene: shader_read → color_attachment, discard",
                      "frame: render post: barrier multisampled: color_attachment → color_attachment; "
                      "scene: color_attachment → shader_read; depth: depth_attachment → depth_attachment",
                      "frame: copy output → readback: barrier output: color_attachment → transfer_src "
                      "(folded); readback: home → transfer_dst",
                      "frame: end: barrier output: transfer_src → color_attachment; readback: transfer_dst "
                      "→ home",
                      "submit frame: first use: multisampled undefined → color_attachment, depth undefined "
                      "→ depth_attachment, scene undefined → shader_read, output undefined → "
                      "color_attachment; ticket 1",
                  }));
    }

    TEST_F(vulkan, mipmaps_are_generated_down_to_one_pixel_and_a_blit_scales_an_image) {
        const auto queue = device().queue();
        const auto texture =
            vk::image::make(device(), {
                                          .format = vk::format::r8g8b8a8_unorm,
                                          .extent = {.width = 8, .height = 8},
                                          .usage = vk::image_usage::sampled | vk::image_usage::transfer_src |
                                                   vk::image_usage::transfer_dst,
                                          .mip_levels = vk::mip_count({.width = 8, .height = 8}),
                                          .name = "texture",
                                      });
        const auto small = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 2, .height = 2},
                          .usage = vk::image_usage::transfer_src | vk::image_usage::transfer_dst,
                          .name = "small",
                      });
        const auto smallest = vk::buffer::make(device(), {
                                                             .size = sizeof(pixel),
                                                             .usage = vk::buffer_usage::transfer_dst,
                                                             .host = vk::host_access::read_write,
                                                             .name = "smallest",
                                                         });
        const auto halves = vk::buffer::make(device(), {
                                                           .size = 4 * sizeof(pixel),
                                                           .usage = vk::buffer_usage::transfer_dst,
                                                           .host = vk::host_access::read_write,
                                                           .name = "halves",
                                                       });
        ASSERT_TRUE(texture) << texture.error();
        ASSERT_TRUE(small) << small.error();
        ASSERT_TRUE(smallest) << smallest.error();
        ASSERT_TRUE(halves) << halves.error();

        auto pattern = std::array<pixel, 8 * 8>{};
        for (auto i = 0uz; i < pattern.size(); i++) {
            pattern[i] = i % 8 < 4 ? pixel{200, 40, 0, 255} : pixel{100, 80, 0, 255};
        }

        auto mips = queue.record("mips");
        mips.upload(*texture, std::as_bytes(std::span{pattern}));
        mips.generate_mipmaps(*texture);
        mips.copy(*texture, *smallest, {.mip = 3});

        const auto generated = queue.submit(std::move(mips));
        ASSERT_TRUE(generated) << generated.error();
        ASSERT_TRUE(device().wait(*generated));

        EXPECT_EQ(read<pixel>(*smallest), (std::vector{pixel{150, 60, 0, 255}}));
        EXPECT_EQ(
            take_trace(),
            (std::vector<std::string>{
                "mips: upload texture: staging buffer, 256 bytes",
                "mips: upload texture: barrier texture mip 0: shader_read → transfer_dst, discard",
                "mips: upload texture: copy staging → texture mip 0",
                "mips: generate_mipmaps texture: barrier texture mip 0: transfer_dst → transfer_src "
                "(folded); texture mips 1-3: shader_read → transfer_dst, discard",
                "mips: generate_mipmaps texture: blit mip 0 → 1",
                "mips: generate_mipmaps texture: barrier texture mip 1: transfer_dst → transfer_src",
                "mips: generate_mipmaps texture: blit mip 1 → 2",
                "mips: generate_mipmaps texture: barrier texture mip 2: transfer_dst → transfer_src",
                "mips: generate_mipmaps texture: blit mip 2 → 3",
                "mips: copy texture → smallest: barrier texture mips 0-2: transfer_src → shader_read; "
                "texture mip 3: transfer_dst → transfer_src (folded); smallest: home → transfer_dst",
                "mips: end: barrier texture mip 3: transfer_src → shader_read; smallest: transfer_dst → "
                "home",
                "submit mips: first use: texture undefined → shader_read, small undefined → transfer_src; "
                "ticket 1",
                "collect: freed staging",
            }));

        auto scale = queue.record("scale");
        scale.blit(*texture, *small);
        scale.copy(*small, *halves);

        const auto scaled = queue.submit(std::move(scale));
        ASSERT_TRUE(scaled) << scaled.error();
        ASSERT_TRUE(device().wait(*scaled));

        EXPECT_EQ(read<pixel>(*halves), (std::vector{
                                            pixel{200, 40, 0, 255},
                                            pixel{100, 80, 0, 255},
                                            pixel{200, 40, 0, 255},
                                            pixel{100, 80, 0, 255},
                                        }));
        EXPECT_EQ(take_trace(),
                  (std::vector<std::string>{
                      "scale: blit texture mip 0 → small: barrier texture mip 0: shader_read → transfer_src; "
                      "small: transfer_src → transfer_dst, discard",
                      "scale: copy small → halves: barrier texture mip 0: transfer_src → shader_read; small: "
                      "transfer_dst → transfer_src; halves: home → transfer_dst",
                      "scale: end: barrier halves: transfer_dst → home",
                      "submit scale: ticket 2",
                  }));
    }

    TEST_F(vulkan, a_draw_fails_at_submit_when_a_binding_its_pipeline_reads_refers_to_a_destroyed_image) {
        const auto queue = device().queue();
        const auto target = vk::image::make(device(), {
                                                          .format = vk::format::r8g8b8a8_unorm,
                                                          .extent = {.width = 16, .height = 16},
                                                          .usage = vk::image_usage::color_attachment,
                                                          .name = "target",
                                                      });
        const auto vertices = green_triangle(device());
        const auto sampler = vk::sampler::make(device());
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(vertices) << vertices.error();
        ASSERT_TRUE(sampler) << sampler.error();

        const auto layout = vk::set_layout::make(
            device(), {.bindings = {{.binding = 0, .type = vk::descriptor_type::combined_image_sampler}}});
        ASSERT_TRUE(layout) << layout.error();
        auto material = vk::descriptor_set::make(device(), {.layout = *layout, .name = "material"});
        ASSERT_TRUE(material) << material.error();
        {
            const auto albedo = vk::image::make(device(), {
                                                              .format = vk::format::r8g8b8a8_unorm,
                                                              .extent = {.width = 16, .height = 16},
                                                              .usage = vk::image_usage::sampled,
                                                              .name = "albedo",
                                                          });
            ASSERT_TRUE(albedo) << albedo.error();
            ASSERT_TRUE(material->write({{0, *albedo, *sampler}}));
        }

        const auto triangle_vertex = shaders::triangle::vertex();
        const auto triangle_fragment = shaders::triangle::fragment();
        const auto post_vertex = shaders::post::vertex();
        const auto post_fragment = shaders::post::fragment();
        ASSERT_TRUE(triangle_vertex) << triangle_vertex.error();
        ASSERT_TRUE(triangle_fragment) << triangle_fragment.error();
        ASSERT_TRUE(post_vertex) << post_vertex.error();
        ASSERT_TRUE(post_fragment) << post_fragment.error();

        const auto plain = vk::graphics_pipeline::make(
            device(), {
                          .vertex = *triangle_vertex,
                          .fragment = *triangle_fragment,
                          .vertex_input = {vk::vertex_layout(&vertex::position, &vertex::color)},
                          .color = {{.format = target->format()}},
                          .sets = {*layout},
                          .push_constant_size = 4 * sizeof(float),
                      });
        const auto sampling =
            vk::graphics_pipeline::make(device(), {
                                                      .vertex = *post_vertex,
                                                      .fragment = *post_fragment,
                                                      .color = {{.format = target->format()}},
                                                      .sets = {*layout},
                                                      .push_constant_size = 4 * sizeof(float),
                                                  });
        ASSERT_TRUE(plain) << plain.error();
        ASSERT_TRUE(sampling) << sampling.error();

        const auto run = [&](const vk::graphics_pipeline& pipeline) -> std::expected<void, rei::error> {
            auto commands = queue.record();
            {
                auto pass = commands.render({.color = {{.target = *target}}});
                pass.bind(pipeline);
                pass.bind(0, *material);
                pass.push_constants(std::array{1.0f, 1.0f, 1.0f, 1.0f});
                pass.bind_vertex_buffer(0, *vertices);
                pass.draw({.vertices = 3});
            }

            return queue.submit(std::move(commands)).and_then([&](vk::ticket ticket) -> auto {
                return device().wait(ticket);
            });
        };

        ASSERT_TRUE(run(*plain));
        EXPECT_TRUE(has_error(run(*sampling), vk::error_code::invalid_argument,
                              "draw in an unnamed render pass: descriptor set 'material' binding 0 refers "
                              "to image 'albedo', which was destroyed"));

        const auto replacement = vk::image::make(device(), {
                                                               .format = vk::format::r8g8b8a8_unorm,
                                                               .extent = {.width = 16, .height = 16},
                                                               .usage = vk::image_usage::sampled,
                                                               .name = "replacement",
                                                           });
        ASSERT_TRUE(replacement) << replacement.error();
        ASSERT_TRUE(material->write({{0, *replacement, *sampler}}));
        ASSERT_TRUE(run(*sampling));
    }

    TEST_F(vulkan, a_pass_whose_fragment_shader_writes_storage_is_ordered_around_earlier_and_later_work) {
        if (!device().has(vk::feature::fragment_stores_and_atomics)) {
            GTEST_SKIP() << "the device lacks fragment_stores_and_atomics";
        }

        const auto queue = device().queue();
        const auto target = vk::image::make(device(), {
                                                          .format = vk::format::r8g8b8a8_unorm,
                                                          .extent = {.width = 16, .height = 16},
                                                          .usage = vk::image_usage::color_attachment,
                                                          .name = "target",
                                                      });
        const auto vertices = green_triangle(device());
        const auto before = vk::buffer::make(device(), {
                                                           .size = 4 * sizeof(float),
                                                           .usage = vk::buffer_usage::transfer_dst,
                                                           .host = vk::host_access::read_write,
                                                           .name = "before",
                                                       });
        const auto after = vk::buffer::make(device(), {
                                                          .size = 4 * sizeof(float),
                                                          .usage = vk::buffer_usage::transfer_dst,
                                                          .host = vk::host_access::read_write,
                                                          .name = "after",
                                                      });
        auto marks = vk::buffer::make(
            device(), {
                          .size = 4 * sizeof(float),
                          .usage = vk::buffer_usage::storage_buffer | vk::buffer_usage::transfer_src,
                          .host = vk::host_access::read_write,
                          .name = "marks",
                      });
        auto arguments = vk::buffer::make(device(), {
                                                        .size = 4 * sizeof(std::uint32_t),
                                                        .usage = vk::buffer_usage::indirect_buffer,
                                                        .host = vk::host_access::write,
                                                        .name = "arguments",
                                                    });
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(vertices) << vertices.error();
        ASSERT_TRUE(before) << before.error();
        ASSERT_TRUE(after) << after.error();
        ASSERT_TRUE(marks) << marks.error();
        ASSERT_TRUE(arguments) << arguments.error();

        const auto unmarked = std::array{1.0f, 1.0f, 1.0f, 1.0f};
        const auto one_triangle = std::array<std::uint32_t, 4>{3, 1, 0, 0};
        ASSERT_TRUE(marks->write(std::as_bytes(std::span{unmarked})));
        ASSERT_TRUE(arguments->write(std::as_bytes(std::span{one_triangle})));

        const auto triangle_vertex = shaders::triangle::vertex();
        const auto triangle_fragment = shaders::triangle::fragment();
        ASSERT_TRUE(triangle_vertex) << triangle_vertex.error();
        ASSERT_TRUE(triangle_fragment) << triangle_fragment.error();
        const auto reading = vk::graphics_pipeline::make(
            device(), {
                          .vertex = *triangle_vertex,
                          .fragment = *triangle_fragment,
                          .vertex_input = {vk::vertex_layout(&vertex::position, &vertex::color)},
                          .color = {{.format = target->format()}},
                      });
        ASSERT_TRUE(reading) << reading.error();

        auto plain = queue.record("plain");
        {
            auto pass = plain.render({.color = {{.target = *target}}});
            pass.bind(*reading);
            pass.bind_vertex_buffer(0, *vertices);
            pass.draw({.vertices = 3});
        }
        ASSERT_TRUE(queue.submit(std::move(plain)));
        EXPECT_EQ(take_trace(), (std::vector<std::string>{
                                    "plain: end: barrier target: color_attachment → color_attachment",
                                    "submit plain: first use: target undefined → color_attachment; ticket 1",
                                }));

        const auto post_vertex = shaders::post::vertex();
        const auto mark_fragment = shaders::mark::fragment();
        ASSERT_TRUE(post_vertex) << post_vertex.error();
        ASSERT_TRUE(mark_fragment) << mark_fragment.error();
        const auto layout = vk::set_layout::make(
            device(), {
                          .bindings = {{.binding = 0, .type = vk::descriptor_type::storage_buffer}},
                          .pushed = true,
                      });
        ASSERT_TRUE(layout) << layout.error();
        const auto marking =
            vk::graphics_pipeline::make(device(), {
                                                      .vertex = *post_vertex,
                                                      .fragment = *mark_fragment,
                                                      .color = {{.format = target->format()}},
                                                      .sets = {*layout},
                                                      .name = "marking",
                                                  });
        ASSERT_TRUE(marking) << marking.error();

        auto commands = queue.record("mark");
        commands.copy(*marks, *before);
        {
            auto pass = commands.render({.color = {{.target = *target}}});
            pass.bind(*marking);
            pass.push_descriptors(0, {{0, *marks}});
            pass.draw_indirect(*arguments, 0, 1);
        }
        commands.copy(*marks, *after);
        {
            auto pass = commands.render({.color = {{.target = *target}}, .name = "plain"});
            pass.bind(*reading);
            pass.bind_vertex_buffer(0, *vertices);
            pass.draw({.vertices = 3});
        }

        const auto ticket = queue.submit(std::move(commands));
        ASSERT_TRUE(ticket) << ticket.error();
        ASSERT_TRUE(device().wait(*ticket));

        auto earlier = std::array<float, 4>{};
        auto later = std::array<float, 4>{};
        auto marked = std::array<float, 4>{};
        ASSERT_TRUE(before->read(std::as_writable_bytes(std::span{earlier})));
        ASSERT_TRUE(after->read(std::as_writable_bytes(std::span{later})));
        ASSERT_TRUE(marks->read(std::as_writable_bytes(std::span{marked})));
        EXPECT_EQ(earlier, (std::array{1.0f, 1.0f, 1.0f, 1.0f}));
        EXPECT_EQ(later, (std::array{42.0f, 1.0f, 1.0f, 1.0f}));
        EXPECT_EQ(marked, (std::array{42.0f, 1.0f, 1.0f, 1.0f}));
        EXPECT_EQ(
            take_trace(),
            (std::vector<std::string>{
                "mark: copy marks → before: barrier before: home → transfer_dst",
                "mark: render: barrier before: transfer_dst → home; memory: earlier work → render",
                "mark: copy marks → after: barrier target: color_attachment → color_attachment; memory: "
                "render writes → later work; after: home → transfer_dst",
                "mark: render plain: barrier after: transfer_dst → home; memory: earlier work → render",
                "mark: end: barrier target: color_attachment → color_attachment",
                "submit mark: ticket 2",
            }));
    }

    TEST_F(vulkan, a_graphics_pipeline_that_does_not_fit_its_shaders_is_refused_and_the_input_is_named) {
        const auto vertex_shader = shaders::triangle::vertex();
        const auto fragment_shader = shaders::triangle::fragment();
        const auto post_vertex = shaders::post::vertex();
        const auto post_fragment = shaders::post::fragment();
        ASSERT_TRUE(vertex_shader) << vertex_shader.error();
        ASSERT_TRUE(fragment_shader) << fragment_shader.error();
        ASSERT_TRUE(post_vertex) << post_vertex.error();
        ASSERT_TRUE(post_fragment) << post_fragment.error();

        const auto whole = vk::vertex_layout(&vertex::position, &vertex::color);
        auto integral = whole;
        integral.attributes[1].format = vk::format::r32_uint;

        EXPECT_TRUE(
            has_error(vk::graphics_pipeline::make(device(),
                                                  {
                                                      .vertex = *vertex_shader,
                                                      .fragment = *fragment_shader,
                                                      .vertex_input = {vk::vertex_layout(&vertex::position)},
                                                      .name = "half",
                                                  }),
                      vk::error_code::layout_mismatch,
                      "graphics pipeline 'half': the vertex shader reads location 1 ('color'), and "
                      "vertex_input has no attribute there"));
        EXPECT_TRUE(
            has_error(vk::graphics_pipeline::make(
                          device(), {.vertex = *vertex_shader, .vertex_input = {integral}, .name = "typed"}),
                      vk::error_code::layout_mismatch,
                      "graphics pipeline 'typed': location 1 ('color') is a r32g32b32_sfloat in the vertex "
                      "shader and a r32_uint in vertex_input"));
        EXPECT_TRUE(has_error(vk::graphics_pipeline::make(device(), {.vertex = *fragment_shader}),
                              vk::error_code::invalid_argument,
                              "an unnamed graphics pipeline: the entry point 'main' isn't a vertex shader"));
        EXPECT_TRUE(has_error(vk::graphics_pipeline::make(device(),
                                                          {
                                                              .vertex = *vertex_shader,
                                                              .vertex_input = {whole},
                                                              .depth =
                                                                  vk::depth_output{
                                                                      .format = vk::format::r8g8b8a8_unorm,
                                                                  },
                                                          }),
                              vk::error_code::invalid_argument,
                              "the depth format r8g8b8a8_unorm has no depth"));
        EXPECT_TRUE(has_error(vk::graphics_pipeline::make(device(),
                                                          {
                                                              .vertex = *post_vertex,
                                                              .fragment = *post_fragment,
                                                              .push_constant_size = 4 * sizeof(float),
                                                              .name = "bare",
                                                          }),
                              vk::error_code::layout_mismatch,
                              "graphics pipeline 'bare': the fragment shader uses set 0 binding 0 ('scene'), "
                              "and the pipeline has 0 set layouts"));
        EXPECT_TRUE(has_error(
            vk::graphics_pipeline::make(device(), {.vertex = *post_vertex, .fragment = *post_fragment}),
            vk::error_code::layout_mismatch,
            "the fragment shader's push constants end at byte 16, and push_constant_size is 0"));
    }

    TEST_F(vulkan, misuse_of_a_render_pass_and_of_blits_surfaces_with_names_and_nothing_is_submitted) {
        const auto queue = device().queue();
        const auto target = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .name = "target",
                      });
        const auto sampled = vk::image::make(device(), {
                                                           .format = vk::format::r8g8b8a8_unorm,
                                                           .extent = {.width = 16, .height = 16},
                                                           .usage = vk::image_usage::sampled,
                                                           .name = "sampled",
                                                       });
        const auto wide = vk::image::make(
            device(), {
                          .format = vk::format::r8g8b8a8_unorm,
                          .extent = {.width = 16, .height = 16},
                          .usage = vk::image_usage::color_attachment | vk::image_usage::transfer_src,
                          .mip_levels = 2,
                          .name = "wide",
                      });
        const auto vertices = green_triangle(device());
        ASSERT_TRUE(target) << target.error();
        ASSERT_TRUE(sampled) << sampled.error();
        ASSERT_TRUE(wide) << wide.error();
        ASSERT_TRUE(vertices) << vertices.error();

        const auto vertex_shader = shaders::triangle::vertex();
        const auto fragment_shader = shaders::triangle::fragment();
        ASSERT_TRUE(vertex_shader) << vertex_shader.error();
        ASSERT_TRUE(fragment_shader) << fragment_shader.error();
        const auto options = vk::graphics_pipeline_options{
            .vertex = *vertex_shader,
            .fragment = *fragment_shader,
            .vertex_input = {vk::vertex_layout(&vertex::position, &vertex::color)},
            .color = {{.format = vk::format::r8g8b8a8_unorm}},
            .name = "triangle",
        };
        auto deep_options = options;
        deep_options.depth = vk::depth_output{.format = vk::format::d32_sfloat};
        deep_options.name = "deep";
        const auto pipeline = vk::graphics_pipeline::make(device(), options);
        const auto deep = vk::graphics_pipeline::make(device(), deep_options);
        ASSERT_TRUE(pipeline) << pipeline.error();
        ASSERT_TRUE(deep) << deep.error();

        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *target}}, .name = "empty"});
                pass.draw({.vertices = 3});
            },
            "draw in render pass 'empty': no pipeline is bound"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *target}}});
                pass.bind(*pipeline);
                pass.draw({.vertices = 3});
            },
            "draw in an unnamed render pass: graphics pipeline 'triangle' reads vertex buffer 0, and none "
            "is bound there"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *target}}});
                pass.bind(*pipeline);
                pass.bind_vertex_buffer(0, *vertices);
                pass.draw_indexed({.indices = 3});
            },
            "draw_indexed in an unnamed render pass: no index buffer is bound"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *target}}});
                pass.bind(*deep);
            },
            "bind of graphics pipeline 'deep' in an unnamed render pass: it was made for color "
            "[r8g8b8a8_unorm], depth d32_sfloat, 1 samples, and the pass has color [r8g8b8a8_unorm], depth "
            "none, 1 samples"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *sampled}}});
            },
            "render into image 'sampled' in an unnamed render pass: its usage lacks color_attachment"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *wide}}});
            },
            "render into image 'wide' in an unnamed render pass: a target is a view of one mip level of a "
            "2D image, and this one has 2 levels and a depth of 1"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({
                    .color = {{.target = *target, .resolve = vk::resolve_target{*wide}}},
                });
            },
            "resolve into image 'wide' in an unnamed render pass: a target is a view of one mip level"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({
                    .color = {{.target = *target}},
                    .area = vk::area{.x = 8, .width = 16, .height = 16},
                });
            },
            "an unnamed render pass: the area 16x16 at 8,0 isn't inside its targets of 16x16"));
        EXPECT_TRUE(
            misuse([&](vk::commands& commands) -> void { auto pass = commands.render({.name = "bare"}); },
                   "render pass 'bare': it has 0 color targets and no depth target"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *target}}, .name = "open"});
                commands.generate_mipmaps(*wide);
            },
            "generate_mipmaps: render pass 'open' is still open"));
        EXPECT_TRUE(misuse(
            [&](vk::commands& commands) -> void {
                auto pass = commands.render({.color = {{.target = *target}}});
                pass.end();
                pass.draw({.vertices = 3});
            },
            "draw in a render pass that has ended"));
        EXPECT_TRUE(
            misuse([&](vk::commands& commands) -> void { commands.generate_mipmaps(*wide); },
                   "generate_mipmaps of image 'wide': it needs the transfer_src and transfer_dst usages"));
        EXPECT_TRUE(
            misuse([&](vk::commands& commands) -> void { commands.blit(*target, *target); },
                   "blit from image 'target' into image 'target': the source needs the transfer_src usage, "
                   "the destination transfer_dst"));

        auto unfinished = queue.record("unfinished");
        auto pass = unfinished.render({.color = {{.target = *target}}, .name = "open"});
        EXPECT_TRUE(has_error(queue.submit(std::move(unfinished)), vk::error_code::invalid_argument,
                              "submit of unfinished: render pass 'open' is still open"));
        pass.viewport({.width = 8, .height = 8});
        pass.draw({.vertices = 3});
        pass.end();
    }
} // namespace rei::gpu::tests
