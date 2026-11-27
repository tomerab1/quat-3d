// Clustered point/spot lighting renders (R10.1): the deferred lighting and
// forward transparent passes shading through the light-cull pass's lists.
// GpuClusteredLighting.PointLightLitWithinRadiusDarkBeyond replaces the old
// run_point_light_self_test (now against an exact per-pixel PBR expectation).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <random>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/light_clusters.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/transparent_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "renderer/pbr_reference.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
using engine::core::Error;

glm::mat4 vulkan_perspective(float fov_y, float aspect, float near_z, float far_z) {
    glm::mat4 p = glm::perspective(fov_y, aspect, near_z, far_z);
    p[1][1] *= -1.0F;
    return p;
}

// A quad (two triangles) with corners origin, origin+u, origin+u+v, origin+v.
std::expected<asset::MeshAsset, Error> upload_quad(quat::test::GpuContext& g,
                                                   const glm::vec3& origin, const glm::vec3& u,
                                                   const glm::vec3& v) {
    const glm::vec3 n = glm::normalize(glm::cross(u, v));
    std::array<asset::Vertex, 4> verts{};
    const std::array<glm::vec3, 4> pos{origin, origin + u, origin + u + v, origin + v};
    for (std::size_t i = 0; i < 4; ++i) {
        verts[i].position = pos[i];
        verts[i].normal = n;
        verts[i].tangent = glm::vec4(glm::normalize(u), 1.0F);
    }
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    auto vb = rhi::upload_device_buffer(g.allocator, g.transfer, verts.data(), sizeof(verts),
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vb) return std::unexpected(vb.error());
    auto ib = rhi::upload_device_buffer(g.allocator, g.transfer, indices.data(), sizeof(indices),
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ib) return std::unexpected(ib.error());
    asset::MeshAsset mesh;
    mesh.vertex_buffer = std::move(*vb);
    mesh.index_buffer = std::move(*ib);
    mesh.vertex_count = 4;
    mesh.index_count = 6;
    mesh.submeshes = {asset::SubMesh{0, 6, 0}};
    return mesh;
}

std::expected<asset::MaterialAsset, Error> upload_material(quat::test::GpuContext& g,
                                                          const glm::vec4& base, float roughness,
                                                          std::uint32_t flags = 0) {
    asset::PbrMaterialParams p;
    p.base_color_factor = base;
    p.metallic_factor = 0.0F;
    p.roughness_factor = roughness;
    p.flags = flags;
    return asset::upload_material(p, asset::MaterialTextures{}, g.allocator, g.transfer);
}

struct Camera {
    glm::mat4  view{1.0F};
    glm::mat4  proj{1.0F};
    glm::vec3  eye{0.0F};
    VkExtent2D extent{64, 64};

    [[nodiscard]] glm::uvec2 pixel_of(const glm::vec3& world) const {
        const glm::vec4 clip = proj * view * glm::vec4(world, 1.0F);
        const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
        return {static_cast<std::uint32_t>(uv.x * static_cast<float>(extent.width)),
                static_cast<std::uint32_t>(uv.y * static_cast<float>(extent.height))};
    }
};

Camera look_at(const glm::vec3& eye, const glm::vec3& target, VkExtent2D extent, float far_z) {
    Camera c;
    c.eye = eye;
    c.extent = extent;
    c.view = glm::lookAt(eye, target, glm::vec3(0.0F, 1.0F, 0.0F));
    c.proj = vulkan_perspective(glm::radians(60.0F),
                                static_cast<float>(extent.width) /
                                    static_cast<float>(extent.height),
                                0.1F, far_z);
    return c;
}

struct Passes {
    r::MeshPass        mesh;
    r::LightCullPass   cull;
    r::LightingPass    lighting;
    r::TransparentPass transparent;
};

std::expected<Passes, Error> create_passes(quat::test::GpuContext& g) {
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    auto cull = r::LightCullPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!cull) return std::unexpected(cull.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    auto transparent =
        r::TransparentPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!transparent) return std::unexpected(transparent.error());
    return Passes{std::move(*mesh), std::move(*cull), std::move(*lighting),
                  std::move(*transparent)};
}

struct Frame {
    VkExtent2D                 extent{};
    std::vector<glm::vec3>     hdr;
    std::vector<std::uint32_t> grid;    // (offset, count) per cluster
    std::vector<std::uint32_t> indices;
    std::uint32_t              requested = 0;

    [[nodiscard]] glm::vec3 at(glm::uvec2 p) const {
        return hdr[static_cast<std::size_t>(p.y) * extent.width + p.x];
    }
    [[nodiscard]] std::vector<std::uint32_t> list(std::uint32_t cluster) const {
        return {indices.begin() + grid[cluster * 2],
                indices.begin() + grid[cluster * 2] + grid[cluster * 2 + 1]};
    }
};

struct RenderSpec {
    std::span<const r::DrawItem> opaque;
    std::span<const r::DrawItem> transparent;
    std::span<const r::GpuLight> lights;
    bool                         light_transparent = true; // feed the clusters to the forward pass
};

// GBuffer -> light cull -> lighting (-> transparent), sun and ambient off, no
// sky: only the clustered lights contribute. Reads back the HDR image and the
// cull pass's lists.
std::expected<Frame, Error> render(quat::test::GpuContext& g, Passes& passes, const Camera& cam,
                                   const RenderSpec& spec) {
    const VkExtent2D extent = cam.extent;
    const VkDeviceSize hdr_bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 8;
    constexpr VkDeviceSize grid_bytes = r::cluster_count * 2 * sizeof(std::uint32_t);
    constexpr VkDeviceSize index_bytes = r::max_cluster_light_indices * sizeof(std::uint32_t);
    const auto host = [&](VkDeviceSize bytes) {
        return g.allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    };
    auto hdr_rb = host(hdr_bytes);
    auto grid_rb = host(grid_bytes);
    auto index_rb = host(index_bytes);
    auto counter_rb = host(sizeof(std::uint32_t));
    if (!hdr_rb || !grid_rb || !index_rb || !counter_rb) {
        return std::unexpected(Error{"clustered lighting test: read-back allocation failed"});
    }

    r::DirectionalLightParams sun;
    sun.color = {1.0F, 1.0F, 1.0F, 0.0F};
    sun.ambient = {0.0F, 0.0F, 0.0F, 0.0F};
    const glm::mat4 view_proj = cam.proj * cam.view;

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = passes.mesh.add_to_graph(graph, extent, view_proj, spec.opaque);
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto clustered = passes.cull.add_to_graph(graph, cam.view, cam.proj, spec.lights);
    if (!clustered) return std::unexpected(clustered.error());
    auto hdr = passes.lighting.add_to_graph(graph, *gbuffer, extent, sun, glm::inverse(view_proj),
                                            cam.eye, rhi::ResourceHandle{}, glm::mat4(1.0F),
                                            *clustered);
    if (!hdr) return std::unexpected(hdr.error());
    if (!spec.transparent.empty()) {
        auto t = passes.transparent.add_to_graph(
            graph, *hdr, gbuffer->depth, extent, view_proj, sun, cam.eye, spec.transparent, {},
            spec.light_transparent ? *clustered : r::ClusteredLights{});
        if (!t) return std::unexpected(t.error());
    }
    const r::ClusteredLights cl = *clustered;
    const rhi::ResourceHandle hdr_handle = *hdr;
    const VkBuffer hdr_dst = hdr_rb->handle();
    const VkBuffer grid_dst = grid_rb->handle();
    const VkBuffer index_dst = index_rb->handle();
    const VkBuffer counter_dst = counter_rb->handle();
    (void)graph.add_pass("readback", rhi::PassType::transfer)
        .reads(hdr_handle, rhi::ResourceUsage::transfer_src)
        .reads(cl.grid, rhi::ResourceUsage::transfer_src)
        .reads(cl.indices, rhi::ResourceUsage::transfer_src)
        .reads(cl.counter, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, ctx.resolve(hdr_handle).image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_dst, 1, &region);
            const VkBufferCopy gc{0, 0, grid_bytes};
            vkCmdCopyBuffer(cmd, ctx.resolve(cl.grid).buffer, grid_dst, 1, &gc);
            const VkBufferCopy ic{0, 0, index_bytes};
            vkCmdCopyBuffer(cmd, ctx.resolve(cl.indices).buffer, index_dst, 1, &ic);
            const VkBufferCopy cc{0, 0, sizeof(std::uint32_t)};
            vkCmdCopyBuffer(cmd, ctx.resolve(cl.counter).buffer, counter_dst, 1, &cc);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        quat::test::transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());

    Frame f;
    f.extent = extent;
    const auto* halfs = static_cast<const std::uint16_t*>(hdr_rb->mapped());
    f.hdr.resize(static_cast<std::size_t>(extent.width) * extent.height);
    for (std::size_t i = 0; i < f.hdr.size(); ++i) {
        f.hdr[i] = {glm::unpackHalf1x16(halfs[i * 4 + 0]), glm::unpackHalf1x16(halfs[i * 4 + 1]),
                    glm::unpackHalf1x16(halfs[i * 4 + 2])};
    }
    f.grid.resize(r::cluster_count * 2);
    std::memcpy(f.grid.data(), grid_rb->mapped(), grid_bytes);
    f.indices.resize(r::max_cluster_light_indices);
    std::memcpy(f.indices.data(), index_rb->mapped(), index_bytes);
    std::memcpy(&f.requested, counter_rb->mapped(), sizeof(std::uint32_t));
    return f;
}

float quant8(float v) { return std::round(v * 255.0F) / 255.0F; }

// The pre-R10.1 point-light self test, through the clustered path: a surface
// is lit by a point light within its radius (matching the CPU PBR reference
// with the windowed inverse-square falloff) and black beyond it.
TEST(GpuClusteredLighting, PointLightLitWithinRadiusDarkBeyond) {
    QUAT_REQUIRE_GPU(g);
    auto passes = create_passes(*g);
    ASSERT_OK(passes);
    // Facing +Z at z = 0, viewed head-on from z = 3.
    auto quad = upload_quad(*g, {-2.0F, -2.0F, 0.0F}, {4.0F, 0.0F, 0.0F}, {0.0F, 4.0F, 0.0F});
    ASSERT_OK(quad);
    const glm::vec3 base(0.25F, 0.5F, 0.75F);
    auto mat = upload_material(*g, glm::vec4(base, 1.0F), 1.0F);
    ASSERT_OK(mat);
    const r::DrawItem item{&*quad, &*mat, glm::mat4(1.0F)};
    const Camera cam = look_at({0.0F, 0.0F, 3.0F}, glm::vec3(0.0F), {64, 64}, 100.0F);

    const glm::vec3 light_pos(0.0F, 0.0F, 1.0F);
    const glm::vec3 light_color(1.0F);
    constexpr float intensity = 4.0F;
    const auto render_with = [&](float radius) {
        const r::GpuLight l = r::make_point_light(light_pos, radius, light_color, intensity);
        return render(*g, *passes, cam, {{&item, 1}, {}, {&l, 1}});
    };

    auto lit = render_with(5.0F);
    ASSERT_OK(lit);
    const glm::uvec2 c(32, 32);
    const glm::vec3 px = lit->at(c);
    EXPECT_GT(px.b, 0.01F);
    EXPECT_GT(px.b, px.g); // dielectric diffuse keeps the base-colour ordering
    EXPECT_GT(px.g, px.r);

    // Exact expectation: the pixel centre's ray hits the plane at P.
    const glm::vec2 ndc = (glm::vec2(c) + 0.5F) / 64.0F * 2.0F - 1.0F;
    const glm::vec4 h = glm::inverse(cam.proj * cam.view) * glm::vec4(ndc, 0.5F, 1.0F);
    const glm::vec3 dir = glm::normalize(glm::vec3(h) / h.w - cam.eye);
    const glm::vec3 p = cam.eye + dir * (-cam.eye.z / dir.z);
    const float dist = glm::distance(light_pos, p);
    const float t = std::clamp(1.0F - std::pow(dist / 5.0F, 4.0F), 0.0F, 1.0F);
    const float att = t * t / (dist * dist + 1.0F);
    const glm::vec3 expected = r::pbr_ref::direct(
        glm::vec3(quant8(base.x), quant8(base.y), quant8(base.z)), 0.0F, 1.0F,
        glm::vec3(0.0F, 0.0F, 1.0F), glm::normalize(cam.eye - p), glm::normalize(light_pos - p),
        light_color, intensity * att);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(px[k], expected[k], 0.02F * expected[k] + 2e-3F);

    // Shrunk so the surface is outside the radius: no contribution at all.
    auto dark = render_with(0.5F);
    ASSERT_OK(dark);
    const glm::vec3 d = dark->at(c);
    EXPECT_LE(d.r + d.g + d.b, 1e-4F);
}

// Thousands of lights, all far from a lit floor: they add no cost to the
// floor's clusters (the lists hold only the two nearby lights) and no light,
// while the nearby lights still light their spots exactly as when alone.
TEST(GpuClusteredLighting, FarLightsAddNoCostNearbyLightsStillLight) {
    QUAT_REQUIRE_GPU(g);
    auto passes = create_passes(*g);
    ASSERT_OK(passes);
    auto floor = upload_quad(*g, {-10.0F, 0.0F, 2.0F}, {20.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -24.0F});
    ASSERT_OK(floor);
    auto mat = upload_material(*g, glm::vec4(0.8F, 0.8F, 0.8F, 1.0F), 0.9F);
    ASSERT_OK(mat);
    const r::DrawItem item{&*floor, &*mat, glm::mat4(1.0F)};
    const Camera cam =
        look_at({0.0F, 3.0F, 4.0F}, {0.0F, 0.0F, -4.0F}, {128, 72}, 60.0F);

    std::vector<r::GpuLight> near_lights{
        r::make_point_light({-2.5F, 0.6F, -3.0F}, 2.0F, {1.0F, 0.1F, 0.1F}, 6.0F),
        r::make_point_light({2.5F, 0.6F, -3.0F}, 2.0F, {0.1F, 0.1F, 1.0F}, 6.0F)};
    std::vector<r::GpuLight> all = near_lights;
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> x(-40.0F, 40.0F);
    std::uniform_real_distribution<float> rad(0.5F, 5.0F);
    std::uniform_real_distribution<float> beyond(-130.0F, -100.0F); // past the far plane
    std::uniform_real_distribution<float> behind(16.0F, 30.0F);     // behind the camera
    while (all.size() < r::max_lights) {
        const float z = (all.size() % 2 == 0) ? beyond(rng) : behind(rng);
        all.push_back(r::make_point_light({x(rng), 1.0F, z}, rad(rng), {1.0F, 1.0F, 1.0F}, 50.0F));
    }

    auto with_all = render(*g, *passes, cam, {{&item, 1}, {}, all});
    ASSERT_OK(with_all);
    auto only_near = render(*g, *passes, cam, {{&item, 1}, {}, near_lights});
    ASSERT_OK(only_near);

    const glm::uvec2 p_red = cam.pixel_of({-2.5F, 0.0F, -3.0F});
    const glm::uvec2 p_blue = cam.pixel_of({2.5F, 0.0F, -3.0F});
    const glm::uvec2 p_mid = cam.pixel_of({0.0F, 0.0F, -3.0F}); // 2.57 m from both: unlit
    const glm::vec3 red = with_all->at(p_red);
    const glm::vec3 blue = with_all->at(p_blue);
    EXPECT_GT(red.r, 0.05F);
    EXPECT_GT(red.r, 4.0F * red.b);
    EXPECT_GT(blue.b, 0.05F);
    EXPECT_GT(blue.b, 4.0F * blue.r);
    const glm::vec3 mid = with_all->at(p_mid);
    EXPECT_LE(mid.r + mid.g + mid.b, 1e-4F);
    // The far lights change nothing anywhere on screen.
    for (std::size_t i = 0; i < with_all->hdr.size(); ++i) {
        ASSERT_LE(glm::length(with_all->hdr[i] - only_near->hdr[i]), 1e-3F) << "pixel " << i;
    }

    // Cost: the lit spots' clusters list only their own light, and the whole
    // grid holds exactly the CPU reference's assignment — none of the 4094
    // far lights reached any cluster.
    auto params = r::make_cluster_params(cam.view, cam.proj, r::max_lights);
    ASSERT_OK(params);
    const auto cluster_at = [&](const glm::vec3& world) {
        return r::cluster_index(
            r::cluster_of_view_position(*params, glm::vec3(cam.view * glm::vec4(world, 1.0F))));
    };
    EXPECT_EQ(with_all->list(cluster_at({-2.5F, 0.0F, -3.0F})), std::vector<std::uint32_t>{0});
    EXPECT_EQ(with_all->list(cluster_at({2.5F, 0.0F, -3.0F})), std::vector<std::uint32_t>{1});
    const auto expected = r::assign_lights_to_clusters(*params, all);
    std::uint32_t expected_total = 0;
    for (const auto& l : expected) {
        expected_total += static_cast<std::uint32_t>(l.size());
        for (const std::uint32_t i : l) ASSERT_LT(i, 2U) << "a far light reached a cluster";
    }
    EXPECT_EQ(with_all->requested, expected_total);
    EXPECT_EQ(only_near->requested, expected_total);
}

// Many small lights over a floor: every pixel equals the brute-force sum over
// ALL lights (CPU PBR reference), so no cluster may miss a light that reaches
// its pixels — with lights small next to the froxels, any lookup or AABB error
// drops visible light.
TEST(GpuClusteredLighting, MatchesBruteForceShadingWithManySmallLights) {
    QUAT_REQUIRE_GPU(g);
    auto passes = create_passes(*g);
    ASSERT_OK(passes);
    auto floor = upload_quad(*g, {-12.0F, 0.0F, 4.0F}, {24.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -30.0F});
    ASSERT_OK(floor);
    const glm::vec3 albedo(0.8F); // exact in 8 bits, like roughness 1
    auto mat = upload_material(*g, glm::vec4(albedo, 1.0F), 1.0F);
    ASSERT_OK(mat);
    const r::DrawItem item{&*floor, &*mat, glm::mat4(1.0F)};
    const Camera cam = look_at({0.0F, 2.5F, 5.0F}, {0.0F, 0.0F, -6.0F}, {128, 72}, 80.0F);

    std::mt19937 rng(5);
    std::uniform_real_distribution<float> px(-8.0F, 8.0F);
    std::uniform_real_distribution<float> pz(-22.0F, 3.0F);
    std::uniform_real_distribution<float> py(0.1F, 0.5F);
    std::uniform_real_distribution<float> rad(0.4F, 1.5F);
    std::uniform_real_distribution<float> col(0.2F, 1.0F);
    std::vector<r::GpuLight> lights;
    for (int i = 0; i < 400; ++i) {
        lights.push_back(r::make_point_light({px(rng), py(rng), pz(rng)}, rad(rng),
                                             {col(rng), col(rng), col(rng)}, 3.0F));
    }
    auto f = render(*g, *passes, cam, {{&item, 1}, {}, lights});
    ASSERT_OK(f);

    const glm::mat4 inv_vp = glm::inverse(cam.proj * cam.view);
    const glm::vec3 n(0.0F, 1.0F, 0.0F);
    int lit_pixels = 0;
    int bad = 0;
    for (std::uint32_t y = 0; y < cam.extent.height; ++y) {
        for (std::uint32_t x = 0; x < cam.extent.width; ++x) {
            const glm::vec2 ndc =
                (glm::vec2(x, y) + 0.5F) / glm::vec2(cam.extent.width, cam.extent.height) * 2.0F -
                1.0F;
            const glm::vec4 h = inv_vp * glm::vec4(ndc, 0.5F, 1.0F);
            const glm::vec3 dir = glm::normalize(glm::vec3(h) / h.w - cam.eye);
            if (dir.y >= -1e-3F) continue;
            const glm::vec3 p = cam.eye + dir * (-cam.eye.y / dir.y);
            if (std::abs(p.x) > 11.5F || p.z > 3.5F || p.z < -25.5F) continue;
            glm::vec3 expected(0.0F);
            for (const r::GpuLight& l : lights) {
                const glm::vec3 lp(l.position_range);
                const float d = glm::distance(lp, p);
                if (d >= l.position_range.w) continue;
                const float t = std::clamp(1.0F - std::pow(d / l.position_range.w, 4.0F), 0.0F,
                                           1.0F);
                expected += r::pbr_ref::direct(albedo, 0.0F, 1.0F, n, -dir, glm::normalize(lp - p),
                                               glm::vec3(l.color_intensity),
                                               l.color_intensity.w * t * t / (d * d + 1.0F));
            }
            const glm::vec3 got = f->at({x, y});
            if (expected.r + expected.g + expected.b > 0.01F) ++lit_pixels;
            // Depth-reconstruction error moves the shaded point slightly, so
            // allow a few percent; a missing light is far outside this.
            const glm::vec3 tol = expected * 0.04F + 4e-3F;
            if (glm::any(glm::greaterThan(glm::abs(got - expected), tol))) {
                if (++bad <= 5) {
                    ADD_FAILURE() << "pixel (" << x << "," << y << ") got (" << got.r << ","
                                  << got.g << "," << got.b << ") want (" << expected.r << ","
                                  << expected.g << "," << expected.b << ")";
                }
            }
        }
    }
    EXPECT_EQ(bad, 0);
    EXPECT_GT(lit_pixels, 500); // the lights really cover a good part of the floor
}

// Forward-blended surfaces are lit by the clustered lights too.
TEST(GpuClusteredLighting, TransparentSurfaceIsLitByClusteredLights) {
    QUAT_REQUIRE_GPU(g);
    auto passes = create_passes(*g);
    ASSERT_OK(passes);
    auto wall = upload_quad(*g, {-3.0F, -3.0F, -1.0F}, {6.0F, 0.0F, 0.0F}, {0.0F, 6.0F, 0.0F});
    auto panel = upload_quad(*g, {-1.0F, -1.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F});
    ASSERT_OK(wall);
    ASSERT_OK(panel);
    auto dark = upload_material(*g, glm::vec4(0.05F, 0.05F, 0.05F, 1.0F), 0.9F);
    auto glass = upload_material(*g, glm::vec4(0.9F, 0.9F, 0.9F, 0.5F), 0.6F,
                                 asset::material_blend);
    ASSERT_OK(dark);
    ASSERT_OK(glass);
    const r::DrawItem wall_item{&*wall, &*dark, glm::mat4(1.0F)};
    const r::DrawItem panel_item{&*panel, &*glass, glm::mat4(1.0F)};
    const Camera cam = look_at({0.0F, 0.0F, 3.0F}, glm::vec3(0.0F), {64, 64}, 100.0F);
    const r::GpuLight light = r::make_point_light({0.0F, 0.0F, 0.8F}, 3.0F, {1.0F, 0.0F, 0.0F}, 5.0F);

    RenderSpec spec{{&wall_item, 1}, {&panel_item, 1}, {&light, 1}};
    auto lit = render(*g, *passes, cam, spec);
    ASSERT_OK(lit);
    spec.light_transparent = false;
    auto unlit = render(*g, *passes, cam, spec);
    ASSERT_OK(unlit);

    const glm::uvec2 c(32, 32);
    // The red light adds red to the panel; green/blue are unchanged.
    EXPECT_GT(lit->at(c).r - unlit->at(c).r, 0.05F);
    EXPECT_NEAR(lit->at(c).g, unlit->at(c).g, 1e-3F);
    EXPECT_NEAR(lit->at(c).b, unlit->at(c).b, 1e-3F);
}

// Spot lights share the record and path: lit inside the cone, dark outside it
// (though within range).
TEST(GpuClusteredLighting, SpotLightLightsOnlyInsideItsCone) {
    QUAT_REQUIRE_GPU(g);
    auto passes = create_passes(*g);
    ASSERT_OK(passes);
    auto floor = upload_quad(*g, {-6.0F, 0.0F, 3.0F}, {12.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -12.0F});
    ASSERT_OK(floor);
    auto mat = upload_material(*g, glm::vec4(0.8F, 0.8F, 0.8F, 1.0F), 0.9F);
    ASSERT_OK(mat);
    const r::DrawItem item{&*floor, &*mat, glm::mat4(1.0F)};
    const Camera cam = look_at({0.0F, 4.0F, 4.0F}, {0.0F, 0.0F, -2.0F}, {64, 64}, 50.0F);
    // Straight down from 2 m, 20-25 degree cone: ~0.9 m spot radius.
    const r::GpuLight spot =
        r::make_spot_light({0.0F, 2.0F, -2.0F}, {0.0F, -1.0F, 0.0F}, 6.0F, glm::radians(20.0F),
                           glm::radians(25.0F), glm::vec3(1.0F), 10.0F);
    auto f = render(*g, *passes, cam, {{&item, 1}, {}, {&spot, 1}});
    ASSERT_OK(f);
    const glm::vec3 inside = f->at(cam.pixel_of({0.0F, 0.0F, -2.0F}));
    const glm::vec3 outside = f->at(cam.pixel_of({2.0F, 0.0F, -2.0F})); // 45 deg off-axis
    EXPECT_GT(inside.r, 0.05F);
    EXPECT_LE(outside.r + outside.g + outside.b, 1e-4F);
}

} // namespace
