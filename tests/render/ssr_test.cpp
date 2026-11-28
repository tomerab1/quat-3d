// Screen-space reflections (R21) in a small deferred frame: a mirror floor
// and a sunlit red panel standing on it, under a dark blue HDRI. Where the
// panel's reflection lands on the floor, SSR must show red (the previous
// frame's lit panel, found by the Hi-Z march); with SSR off, and on floor
// pixels whose reflection ray leaves the screen, the floor shows the blue
// environment. The frame runs several times: the first has no previous
// colour yet, the later ones accumulate history.

#include <array>
#include <cstdint>
#include <expected>
#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/hdr_decode.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/hdri_environment.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/ssr_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;

constexpr VkExtent2D k_extent{96, 96};
const glm::vec3 k_camera{0.0F, 1.5F, 4.0F};
const glm::vec3 k_blue{0.0F, 0.0F, 0.2F};

// Two quads: the floor (y = 0, +-5 m) and the red panel (z = -1, facing +Z).
struct Scene {
    std::optional<asset::MeshAsset>     floor_mesh, panel_mesh;
    std::optional<asset::MaterialAsset> mirror, red;
    std::optional<r::HdriEnvironment>   env;
};

std::expected<asset::MeshAsset, Error> quad(quat::test::GpuContext& g,
                                            const std::array<glm::vec3, 4>& corners,
                                            const glm::vec3& normal) {
    std::array<asset::Vertex, 4> verts{};
    for (std::size_t i = 0; i < 4; ++i) {
        verts[i].position = corners[i];
        verts[i].normal = normal;
    }
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    return quat::test::upload_mesh(g, verts, indices);
}

std::expected<asset::MaterialAsset, Error> material(quat::test::GpuContext& g, glm::vec3 color,
                                                    float metallic, float roughness) {
    asset::PbrMaterialParams p;
    p.base_color_factor = {color, 1.0F};
    p.metallic_factor = metallic;
    p.roughness_factor = roughness;
    p.flags |= asset::material_double_sided;
    return asset::upload_material(p, asset::MaterialTextures{}, g.allocator, g.transfer);
}

std::expected<Scene, Error> make_scene(quat::test::GpuContext& g) {
    Scene s;
    auto floor = quad(g, {{{-5, 0, 5}, {5, 0, 5}, {5, 0, -5}, {-5, 0, -5}}}, {0, 1, 0});
    auto panel = quad(g, {{{-1, 0, -1}, {1, 0, -1}, {1, 1.5F, -1}, {-1, 1.5F, -1}}}, {0, 0, 1});
    auto mirror = material(g, glm::vec3(1.0F), 1.0F, 0.0F);
    auto red = material(g, {1.0F, 0.05F, 0.05F}, 0.0F, 0.8F);
    if (!floor || !panel || !mirror || !red) return std::unexpected(Error{"scene upload failed"});
    s.floor_mesh.emplace(std::move(*floor));
    s.panel_mesh.emplace(std::move(*panel));
    s.mirror.emplace(std::move(*mirror));
    s.red.emplace(std::move(*red));

    asset::HdrImage sky;
    sky.width = 64;
    sky.height = 32;
    sky.pixels = asset::PixelBuffer::allocate(asset::hdr_decoded_bytes(64, 32));
    auto* p = reinterpret_cast<float*>(sky.pixels.data());
    for (std::size_t i = 0; i < 64 * 32; ++i) {
        p[i * 3] = k_blue.r;
        p[i * 3 + 1] = k_blue.g;
        p[i * 3 + 2] = k_blue.b;
    }
    auto env = r::HdriEnvironment::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!env) return std::unexpected(env.error());
    s.env.emplace(std::move(*env));
    if (auto set = s.env->set_image(sky, g.transfer); !set) return std::unexpected(set.error());
    rhi::RenderGraph bake;
    if (auto b = s.env->add_bake_to_graph(bake, {}); !b) return std::unexpected(b.error());
    if (auto c = bake.compile(); !c) return std::unexpected(c.error());
    auto ran = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) { bake.execute(cmd); });
    if (!ran) return std::unexpected(ran.error());
    return s;
}

glm::mat4 view_proj() {
    const glm::mat4 view = glm::lookAt(k_camera, glm::vec3(0.0F, 0.3F, 0.0F), {0, 1, 0});
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F; // Vulkan clip space (y down), as the engine's cameras
    return proj * view;
}

glm::uvec2 pixel_of(const glm::vec3& world) {
    const glm::vec4 c = view_proj() * glm::vec4(world, 1.0F);
    const glm::vec2 uv = glm::vec2(c) / c.w * 0.5F + 0.5F;
    return {static_cast<std::uint32_t>(uv.x * k_extent.width),
            static_cast<std::uint32_t>(uv.y * k_extent.height)};
}

// Renders `frames` frames of the scene; returns the last frame's HDR.
std::expected<quat::test::HostImage, Error> render(quat::test::GpuContext& g, Scene& s,
                                                   bool ssr_on, int frames) {
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    auto hiz = r::HiZPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    auto ssr = r::SsrPass::create(g.device, g.allocator, g.cache, g.shader_dir, 1);
    if (!mesh_pass || !lighting || !hiz || !ssr) return std::unexpected(Error{"pass creation"});
    const glm::mat4 vp = view_proj();
    r::DirectionalLightParams light;
    light.direction = glm::vec4(glm::normalize(glm::vec3(0.0F, 1.0F, 1.0F)), 0.0F);
    light.color = {1.0F, 1.0F, 1.0F, 3.0F};
    r::SsrSettings settings;
    settings.enabled = ssr_on;
    settings.quality = static_cast<int>(r::SsrQuality::high);
    std::expected<quat::test::HostImage, Error> image = std::unexpected(Error{"no frames"});
    for (int frame = 0; frame < frames; ++frame) {
        rhi::TransientImagePool pool(g.device, g.allocator);
        rhi::RenderGraph graph(pool);
        const std::array<r::DrawItem, 2> items{{{&*s.floor_mesh, &*s.mirror, glm::mat4(1.0F)},
                                                {&*s.panel_mesh, &*s.red, glm::mat4(1.0F)}}};
        auto gbuffer = mesh_pass->add_to_graph(graph, k_extent, vp, items);
        if (!gbuffer) return std::unexpected(gbuffer.error());
        auto pyramid = hiz->add_to_graph(graph, gbuffer->depth, k_extent);
        if (!pyramid) return std::unexpected(pyramid.error());
        r::SsrInputs in;
        in.depth = gbuffer->depth;
        in.normal = gbuffer->normal;
        in.material = gbuffer->material;
        in.velocity = gbuffer->velocity;
        in.hiz = *pyramid;
        in.view_proj = vp;
        in.camera_pos = k_camera;
        in.extent = k_extent;
        in.frame_index = static_cast<std::uint64_t>(frame);
        auto reflections = ssr->add_to_graph(graph, 0, in, settings);
        if (!reflections) return std::unexpected(reflections.error());
        r::LightingTuning tuning;
        tuning.reflections = *reflections;
        auto hdr = lighting->add_to_graph(graph, *gbuffer, k_extent, light, glm::inverse(vp),
                                          k_camera, {}, {}, /*enable_sky=*/true, s.env->views(),
                                          VK_NULL_HANDLE, VK_NULL_HANDLE, {}, tuning);
        if (!hdr) return std::unexpected(hdr.error());
        ssr->add_color_copy(graph, *hdr);
        image = quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
        if (!image) return image;
    }
    return image;
}

glm::vec3 at(const quat::test::HostImage& img, glm::uvec2 p) {
    return glm::vec3(img.rgba16f(p.x, p.y));
}

} // namespace

TEST(GpuSsr, MirrorFloorReflectsThePanel) {
    QUAT_REQUIRE_GPU(g);
    auto s = make_scene(*g); // per test: GPU objects die before the shared device
    ASSERT_OK(s);
    // The panel's mid-height point mirrored below the floor, seen from the
    // camera, crosses the floor at z = 4 - 5 * (1.5 / 2.25).
    const glm::uvec2 reflection = pixel_of({0.0F, 0.0F, 4.0F - 5.0F * (1.5F / 2.25F)});
    const glm::uvec2 panel = pixel_of({0.0F, 0.75F, -1.0F});

    auto with = render(*g, *s, true, 4);
    ASSERT_OK(with);
    const glm::vec3 lit_panel = at(*with, panel);
    const glm::vec3 reflected = at(*with, reflection);
    ASSERT_GT(lit_panel.r, 0.1F) << "the panel itself is not lit";
    EXPECT_GT(reflected.r, 0.5F * lit_panel.r) << "no red reflection at " << reflection.x << ","
                                               << reflection.y;
    EXPECT_GT(reflected.r, reflected.b) << "the reflection is the environment's blue";

    auto without = render(*g, *s, false, 1);
    ASSERT_OK(without);
    const glm::vec3 env_only = at(*without, reflection);
    EXPECT_LT(env_only.r, 0.02F);
    EXPECT_GT(env_only.b, 0.1F);
}

// Rays that leave the screen fall back to the environment: the floor near
// the bottom corners reflects the sky behind the camera.
TEST(GpuSsr, MissesFallBackToTheEnvironment) {
    QUAT_REQUIRE_GPU(g);
    auto s = make_scene(*g); // per test: GPU objects die before the shared device
    ASSERT_OK(s);
    auto with = render(*g, *s, true, 3);
    ASSERT_OK(with);
    const glm::vec3 corner = at(*with, {4, k_extent.height - 4});
    EXPECT_LT(corner.r, 0.02F);
    EXPECT_GT(corner.b, 0.1F);
}
