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
#include <memory>
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

std::expected<Passes, Error> make_passes(quat::test::GpuContext& g,
                                         std::shared_ptr<r::CsmShared> shared = nullptr) {
    auto mesh = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh) return std::unexpected(mesh.error());
    auto hiz = r::HiZPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!hiz) return std::unexpected(hiz.error());
    auto shadow =
        r::ShadowPass::create(g.device, g.allocator, g.cache, g.shader_dir, std::move(shared));
    if (!shadow) return std::unexpected(shadow.error());
    auto lighting = r::LightingPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    return Passes{std::move(*mesh), std::move(*hiz), std::move(*shadow), std::move(*lighting)};
}

// GBuffer -> Hi-Z -> cascades -> lighting for `draws`; returns the HDR image
// and what the GPU fitted.
std::expected<Frame, Error> render(quat::test::GpuContext& g, Passes& p,
                                   const occ::Camera& cam, const r::CsmSettings& csm,
                                   std::span<const r::DrawItem> draws,
                                   const glm::vec3& to_light = k_to_light) {
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = p.mesh.add_to_graph(graph, k_extent, cam.view_proj, draws);
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hiz = p.hiz.add_to_graph(graph, gbuffer->depth, k_extent);
    if (!hiz) return std::unexpected(hiz.error());
    const r::CsmView view{cam.view, cam.proj, to_light};
    auto shadows = p.shadow.add_to_graph(graph, view, csm, &*hiz, draws);
    if (!shadows) return std::unexpected(shadows.error());
    r::DirectionalLightParams sun;
    sun.direction = glm::vec4(to_light, 0.0F);
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
    const glm::vec2 range = r::shadow_range(cam.proj, csm, true, lo, hi).range;
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

// SDSM: when the depth-fitted range ends at the farthest geometry, that
// geometry is at the far end of the last cascade — where the shadow-distance
// fade would sit. A back wall receiving an occluder's shadow keeps it.
TEST(GpuCsm, FarthestGeometryKeepsItsShadow) {
    QUAT_REQUIRE_GPU(g);
    const glm::vec3 eye(0.0F, 2.0F, 0.0F);
    // The wall stands just inside a range quantisation step (23.9 -> 24 m),
    // so it lies in the last cascade's blend band.
    constexpr float wall_z = -23.9F;
    const occ::Camera cam = occ::look_at(eye, {0.0F, 2.0F, wall_z}, 60.0F, 1.0F, 0.1F, 200.0F);
    const glm::vec3 to_light = glm::normalize(glm::vec3(0.3F, 0.8F, 1.0F));
    auto floor = occ::upload_box(*g, {-15.0F, -0.5F, wall_z - 0.5F}, {15.0F, 0.0F, 5.0F});
    auto wall = occ::upload_box(*g, {-15.0F, 0.0F, wall_z - 0.5F}, {15.0F, 8.0F, wall_z});
    auto blocker = occ::upload_box(*g, {-0.6F, 4.4F, wall_z + 2.4F}, {0.6F, 5.6F, wall_z + 3.6F});
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(floor);
    ASSERT_OK(wall);
    ASSERT_OK(blocker);
    ASSERT_OK(grey);
    const std::vector<r::DrawItem> draws{{&*floor, &*grey, glm::mat4(1.0F)},
                                         {&*wall, &*grey, glm::mat4(1.0F)},
                                         {&*blocker, &*grey, glm::mat4(1.0F)}};
    auto passes = make_passes(*g);
    ASSERT_OK(passes);
    auto frame = render(*g, *passes, cam, r::CsmSettings{}, draws, to_light);
    ASSERT_OK(frame);
    const r::GpuCascades& cs = frame->gpu;
    ASSERT_EQ(cs.params.w, 1.0F); // fitted to the depth bounds
    EXPECT_LT(cs.split_far[3], cs.limits.x);

    // The blocker's shadow centre on the wall (3 m along -to_light, whose
    // x:y:z is 0.3:0.8:1), and a lit wall point.
    const glm::vec3 shadow(-0.9F, 2.6F, wall_z);
    const glm::vec3 lit(3.0F, shadow.y, wall_z);
    // The receiver lies in the last cascade's blend band (the fade region).
    const float band_start = cs.split_far[3] - cs.params.z * (cs.split_far[3] - cs.split_far[2]);
    EXPECT_GT(-wall_z, band_start);
    const float dark = min_luma(frame->hdr, occ::project(cam.view_proj, shadow, k_extent));
    const float bright = luma(frame->hdr, occ::project(cam.view_proj, lit, k_extent));
    EXPECT_GT(bright, 0.2F);
    EXPECT_LT(dark, 0.35F * bright) << "lit " << bright << " shadow " << dark;
}

// Two frame slots share one map: alternating frames (separate passes, the
// same CsmShared) both render correct shadows; under the validation layer
// the cross-frame import barriers keep sync validation quiet.
TEST(GpuCsm, FrameSlotsShareOneMap) {
    QUAT_REQUIRE_GPU(g);
    auto shared = r::CsmShared::create(g->device, g->allocator);
    ASSERT_OK(shared);
    auto slot_a = make_passes(*g, *shared);
    auto slot_b = make_passes(*g, *shared);
    ASSERT_OK(slot_a);
    ASSERT_OK(slot_b);
    EXPECT_EQ(slot_a->shadow.shadow_image(), slot_b->shadow.shadow_image());
    const occ::Camera cam = occ::look_at(k_eye, k_target, 60.0F, 1.0F, 0.1F, 200.0F);
    const std::array<float, 4> dist{6.0F, 12.0F, 25.0F, 60.0F};
    auto scene = make_scene(*g, cam, &dist);
    ASSERT_OK(scene);
    for (int frame_index = 0; frame_index < 4; ++frame_index) {
        Passes& slot = frame_index % 2 == 0 ? *slot_a : *slot_b;
        auto frame = render(*g, slot, cam, r::CsmSettings{}, scene->draws());
        ASSERT_OK(frame);
        const float dark = min_luma(frame->hdr,
                                    occ::project(cam.view_proj, scene->shadow_probe[1], k_extent));
        const float lit = luma(frame->hdr,
                               occ::project(cam.view_proj, scene->lit_probe[1], k_extent));
        EXPECT_LT(dark, 0.35F * lit) << "frame " << frame_index;
    }
}

// Shadow acne: a sloped, fully lit floor close to the camera (cascade 0) at
// default settings stays as bright as without shadows everywhere in a patch.
// Depth precision of the near cascades (D16 with pancaking) must not eat the
// receiver offsets.
TEST(GpuCsm, SlopedLitFloorHasNoAcneInTheNearCascade) {
    QUAT_REQUIRE_GPU(g);
    auto floor = occ::upload_box(*g, {-6.0F, -0.2F, -6.0F}, {6.0F, 0.0F, 6.0F});
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(floor);
    ASSERT_OK(grey);
    const glm::mat4 slope =
        glm::rotate(glm::mat4(1.0F), glm::radians(25.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    const std::vector<r::DrawItem> draws{{&*floor, &*grey, slope}};
    const occ::Camera cam =
        occ::look_at({0.0F, 1.6F, 2.0F}, {0.0F, 0.0F, 0.0F}, 60.0F, 1.0F, 0.1F, 200.0F);
    // Lit at a fairly shallow angle (N.L about 0.45): the acne-prone case.
    const glm::vec3 to_light = glm::normalize(glm::vec3(0.9F, 0.55F, 0.4F));
    auto passes = make_passes(*g);
    ASSERT_OK(passes);
    r::CsmSettings with;
    r::CsmSettings without;
    without.enabled = false;
    auto shadowed = render(*g, *passes, cam, with, draws, to_light);
    auto reference = render(*g, *passes, cam, without, draws, to_light);
    ASSERT_OK(shadowed);
    ASSERT_OK(reference);
    // Every pixel whose floor point lies inside cascade 0 (ray / plane
    // intersection with the sloped top face).
    const glm::vec3 n = glm::vec3(slope * glm::vec4(0.0F, 1.0F, 0.0F, 0.0F));
    const glm::vec3 fwd = glm::normalize(glm::vec3(0.0F) - cam.position);
    const glm::mat4 inv_vp = glm::inverse(cam.view_proj);
    const float cascade0_end = shadowed->gpu.split_far[0];
    int counted = 0;
    float worst = 1.0F;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const glm::vec2 ndc((static_cast<float>(x) + 0.5F) / k_extent.width * 2.0F - 1.0F,
                                (static_cast<float>(y) + 0.5F) / k_extent.height * 2.0F - 1.0F);
            const glm::vec4 far_p = inv_vp * glm::vec4(ndc, 1.0F, 1.0F);
            const glm::vec3 dir = glm::normalize(glm::vec3(far_p) / far_p.w - cam.position);
            const float t = -glm::dot(cam.position, n) / glm::dot(dir, n);
            const glm::vec3 p = cam.position + dir * t;
            if (t <= 0.0F || glm::length(glm::vec2(p.x, p.z)) > 4.0F) continue;
            if (glm::dot(p - cam.position, fwd) > 0.9F * cascade0_end) continue;
            const float ref = luma(reference->hdr, {x, y});
            ASSERT_GT(ref, 0.1F) << x << "," << y;
            worst = std::min(worst, luma(shadowed->hdr, {x, y}) / ref);
            ++counted;
        }
    }
    ASSERT_GT(counted, 500) << "cascade 0 ends at " << cascade0_end;
    EXPECT_GE(worst, 0.95F);
}
