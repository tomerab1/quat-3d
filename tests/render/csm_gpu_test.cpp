// Cascaded shadow maps on the GPU (R12): the GPU cascade fit (csm_setup.slang,
// from the Hi-Z depth bounds) matches the CPU reference, and an occluder in
// each of the four cascades casts a shadow on the floor, both with GPU-fitted
// and CPU-fitted cascades.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/csm.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
namespace occ = quat::test::occlusion;
using engine::core::Error;

constexpr VkExtent2D k_extent{256, 256};
const glm::vec3      k_eye(0.0F, 3.0F, 0.0F);
const glm::vec3      k_target(0.0F, 0.0F, -20.0F);
const glm::vec3      k_to_light = glm::normalize(glm::vec3(-0.6F, 1.0F, 0.0F));

struct Frame {
    quat::test::HostImage hdr;
    r::GpuCascades        gpu{};
    std::uint32_t         bounds[2] = {0, 0};
};

struct Passes {
    r::MeshPass     mesh;
    r::HiZPass      hiz;
    r::ShadowPass   shadow;
    r::LightingPass lighting;
};

std::expected<Passes, Error> make_passes(quat::test::GpuContext& g) {
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    auto hiz = r::HiZPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!hiz) return std::unexpected(hiz.error());
    auto shadow = r::ShadowPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!shadow) return std::unexpected(shadow.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    return Passes{std::move(*mesh), std::move(*hiz), std::move(*shadow), std::move(*lighting)};
}

// GBuffer -> Hi-Z -> cascades -> lighting for `draws`; returns the HDR image
// and what the GPU fitted.
std::expected<Frame, Error> render(quat::test::GpuContext& g, Passes& p,
                                   const occ::Camera& cam, const r::CsmSettings& csm,
                                   std::span<const r::DrawItem> draws) {
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = p.mesh.add_to_graph(graph, k_extent, cam.view_proj, draws);
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hiz = p.hiz.add_to_graph(graph, gbuffer->depth, k_extent);
    if (!hiz) return std::unexpected(hiz.error());
    const r::CsmView view{cam.view, cam.proj, k_to_light};
    auto shadows = p.shadow.add_to_graph(graph, view, csm, &*hiz, draws);
    if (!shadows) return std::unexpected(shadows.error());
    r::DirectionalLightParams sun;
    sun.direction = glm::vec4(k_to_light, 0.0F);
    sun.color = {1.0F, 1.0F, 1.0F, 3.0F};
    sun.ambient = {0.05F, 0.05F, 0.05F, 0.0F};
    r::LightingOcclusion occlusion;
    occlusion.shadows = *shadows;
    occlusion.csm = csm;
    occlusion.projection = cam.proj;
    auto hdr = p.lighting.add_to_graph(graph, *gbuffer, k_extent, sun,
                                       glm::inverse(cam.view_proj), cam.position, occlusion);
    if (!hdr) return std::unexpected(hdr.error());
    auto image = quat::test::render_to_host(g, graph, *hdr, k_extent, 8);
    if (!image) return std::unexpected(image.error());
    Frame out;
    out.hdr = std::move(*image);
    if (auto inv = p.shadow.cascade_buffer().invalidate_mapped(); !inv) {
        return std::unexpected(inv.error());
    }
    std::memcpy(&out.gpu, p.shadow.cascade_buffer().mapped(), sizeof(out.gpu));
    if (auto inv = p.hiz.bounds_buffer().invalidate_mapped(); !inv) {
        return std::unexpected(inv.error());
    }
    std::memcpy(out.bounds, p.hiz.bounds_buffer().mapped(), sizeof(out.bounds));
    return out;
}

float luma(const quat::test::HostImage& img, glm::uvec2 px) {
    const glm::vec4 c = img.rgba16f(px.x, px.y);
    return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
}

// Darkest pixel within +-1 of `px` (far shadows span few pixel rows).
float min_luma(const quat::test::HostImage& img, glm::uvec2 px) {
    float out = luma(img, px);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            out = std::min(out, luma(img, {px.x + static_cast<std::uint32_t>(dx),
                                           px.y + static_cast<std::uint32_t>(dy)}));
        }
    }
    return out;
}

// The floor point at view distance `d` with x = `x` (the camera has no yaw).
glm::vec3 floor_point(const occ::Camera& cam, float d, float x) {
    const glm::vec3 fwd = glm::normalize(k_target - k_eye);
    const float z = cam.position.z + (d - x * fwd.x + cam.position.y * fwd.y) / fwd.z;
    return {x, 0.0F, z};
}

struct Scene {
    asset::MeshAsset                floor;
    std::vector<asset::MeshAsset>   boxes;
    asset::MaterialAsset            grey;
    std::array<glm::vec3, 4>        shadow_probe{};
    std::array<glm::vec3, 4>        lit_probe{};

    // Built on use: the items point at this object's members.
    [[nodiscard]] std::vector<r::DrawItem> draws() const {
        std::vector<r::DrawItem> out{{&floor, &grey, glm::mat4(1.0F)}};
        for (const asset::MeshAsset& b : boxes) out.push_back({&b, &grey, glm::mat4(1.0F)});
        return out;
    }
};

// Floor, plus (when `occluder_dist` is given) one floating box per cascade
// whose shadow centre lands at view distance occluder_dist[c]. Boxes are
// long along the view axis so distant shadows still cover pixel rows.
std::expected<Scene, Error> make_scene(quat::test::GpuContext& g, const occ::Camera& cam,
                                       const std::array<float, 4>* occluder_dist) {
    Scene s;
    auto floor = occ::upload_box(g, {-80.0F, -0.5F, -160.0F}, {80.0F, 0.0F, 10.0F});
    if (!floor) return std::unexpected(floor.error());
    s.floor = std::move(*floor);
    auto grey = occ::upload_grey(g);
    if (!grey) return std::unexpected(grey.error());
    s.grey = std::move(*grey);
    if (occluder_dist != nullptr) {
        for (std::size_t c = 0; c < 4; ++c) {
            const float d = (*occluder_dist)[c];
            const float half = 0.04F * d;
            const float h = 4.0F * half;
            s.shadow_probe[c] = floor_point(cam, d, 0.6F * h);
            s.lit_probe[c] = floor_point(cam, d, -0.6F * h);
            const glm::vec3 centre(0.0F, h, s.shadow_probe[c].z);
            const glm::vec3 extent(half, half, 3.0F * half);
            auto box = occ::upload_box(g, centre - extent, centre + extent);
            if (!box) return std::unexpected(box.error());
            s.boxes.push_back(std::move(*box));
        }
    }
    return s;
}

std::uint32_t cascade_of(const r::GpuCascades& cs, float view_dist) {
    const auto count = static_cast<std::uint32_t>(cs.params.x);
    for (std::uint32_t c = 0; c < count; ++c) {
        if (view_dist < cs.split_far[static_cast<int>(c)]) return c;
    }
    return count;
}

// Renders the floor alone to learn the cascade splits, then an occluder in
// the middle of each cascade (outside the blend bands), and checks each one
// darkens the floor where its shadow falls.
void expect_shadow_in_every_cascade(quat::test::GpuContext& g, const r::CsmSettings& csm) {
    const occ::Camera cam = occ::look_at(k_eye, k_target, 60.0F, 1.0F, 0.1F, 200.0F);
    auto passes = make_passes(g);
    ASSERT_OK(passes);
    auto empty = make_scene(g, cam, nullptr);
    ASSERT_OK(empty);
    auto first = render(g, *passes, cam, csm, empty->draws());
    ASSERT_OK(first);
    ASSERT_EQ(first->gpu.params.x, 4.0F);

    std::array<float, 4> dist{};
    for (int c = 0; c < 4; ++c) {
        const float start = c == 0 ? first->gpu.params.y : first->gpu.split_far[c - 1];
        dist[static_cast<std::size_t>(c)] = start + 0.45F * (first->gpu.split_far[c] - start);
    }
    // The nearest cascade must start in front of the visible floor.
    dist[0] = std::max(dist[0], 5.0F);
    auto scene = make_scene(g, cam, &dist);
    ASSERT_OK(scene);
    auto frame = render(g, *passes, cam, csm, scene->draws());
    ASSERT_OK(frame);
    const glm::vec3 fwd = glm::normalize(k_target - k_eye);
    for (std::uint32_t c = 0; c < 4; ++c) {
        const glm::vec3 probe = scene->shadow_probe[c];
        EXPECT_EQ(cascade_of(frame->gpu, glm::dot(probe - k_eye, fwd)), c);
        const float dark = min_luma(frame->hdr, occ::project(cam.view_proj, probe, k_extent));
        const float lit =
            luma(frame->hdr, occ::project(cam.view_proj, scene->lit_probe[c], k_extent));
        EXPECT_GT(lit, 0.2F) << "cascade " << c;
        EXPECT_LT(dark, 0.35F * lit) << "cascade " << c << " lit " << lit << " shadow " << dark;
    }
}

} // namespace

TEST(GpuCsm, GpuFitMatchesTheCpuReference) {
    QUAT_REQUIRE_GPU(g);
    const occ::Camera cam = occ::look_at(k_eye, k_target, 60.0F, 1.0F, 0.1F, 200.0F);
    auto passes = make_passes(*g);
    ASSERT_OK(passes);
    auto scene = make_scene(*g, cam, nullptr);
    ASSERT_OK(scene);
    r::CsmSettings csm;
    auto frame = render(*g, *passes, cam, csm, scene->draws());
    ASSERT_OK(frame);
    ASSERT_LE(frame->bounds[0], frame->bounds[1]);

    float lo = 0.0F;
    float hi = 0.0F;
    std::memcpy(&lo, &frame->bounds[0], sizeof(float));
    std::memcpy(&hi, &frame->bounds[1], sizeof(float));
    const glm::vec2 range = r::shadow_range(cam.proj, csm, true, lo, hi);
    // The floor's depth bounds tighten the range below the 150 m maximum.
    EXPECT_LT(range.y, 150.0F);
    const r::GpuCascades cpu = r::fit_cascades({cam.view, cam.proj, k_to_light}, csm, range,
                                                r::shadow_map_size);
    const r::GpuCascades& gpu = frame->gpu;
    EXPECT_EQ(gpu.params.w, 1.0F); // fitted to the depth bounds
    EXPECT_EQ(gpu.params.x, cpu.params.x);
    EXPECT_NEAR(gpu.params.y, cpu.params.y, 1e-4F * cpu.params.y);
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(gpu.split_far[c], cpu.split_far[c], 1e-3F * cpu.split_far[c]);
        EXPECT_NEAR(gpu.texel_world[c], cpu.texel_world[c], 1e-5F * cpu.texel_world[c]);
        EXPECT_NEAR(gpu.depth_range[c], cpu.depth_range[c], 1e-3F * cpu.depth_range[c]);
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                const float want = cpu.view_proj[static_cast<std::size_t>(c)][col][row];
                const float got = gpu.view_proj[static_cast<std::size_t>(c)][col][row];
                EXPECT_NEAR(got, want, 1e-3F * std::max(1.0F, std::abs(want)))
                    << "cascade " << c << " m[" << col << "][" << row << "]";
            }
        }
    }
}

TEST(GpuCsm, OccluderShadowsInEveryCascadeGpuFit) {
    QUAT_REQUIRE_GPU(g);
    expect_shadow_in_every_cascade(*g, r::CsmSettings{});
}

TEST(GpuCsm, OccluderShadowsInEveryCascadeCpuFallback) {
    QUAT_REQUIRE_GPU(g);
    r::CsmSettings csm;
    csm.gpu_depth_bounds = false;
    csm.max_distance = 120.0F;
    expect_shadow_in_every_cascade(*g, csm);
}
