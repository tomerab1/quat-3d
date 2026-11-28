#pragma once

// Shared scene for the material shadow-caster tests (R12.11: alpha-masked and
// translucent casters): a floor, a horizontal caster quad above it, the sun
// at 45 degrees so the quad's shadow lands beside it, and a camera on the
// sun's side looking at that shadow. Renders GBuffer -> Hi-Z -> cascaded
// shadows -> lighting and returns the lit HDR image.

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
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
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace quat::test::shadow_scene {

namespace r = engine::renderer;

inline constexpr VkExtent2D k_extent{256, 256};
// Toward the sun: 45 degrees from +x, so a caster at height h shadows the
// floor h metres toward -x.
inline const glm::vec3 k_to_light = glm::normalize(glm::vec3(1.0F, 1.0F, 0.0F));

// A horizontal quad at height `y` spanning x, z in [-half, half], normal +y,
// uv (0, 0) at (-half, -half) and (1, 1) at (half, half).
[[nodiscard]] inline std::expected<engine::asset::MeshAsset, engine::core::Error>
upload_quad(GpuContext& g, float y, float half) {
    std::vector<engine::asset::Vertex> verts;
    const std::array<glm::vec2, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (const glm::vec2& t : uv) {
        engine::asset::Vertex v;
        v.position = {(t.x * 2.0F - 1.0F) * half, y, (t.y * 2.0F - 1.0F) * half};
        v.normal = {0.0F, 1.0F, 0.0F};
        v.uv = t;
        v.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
        verts.push_back(v);
    }
    const std::vector<std::uint32_t> indices{0, 2, 1, 0, 3, 2};
    return upload_mesh(g, verts, indices);
}

[[nodiscard]] inline std::expected<engine::asset::MaterialAsset, engine::core::Error>
upload_params(GpuContext& g, const engine::asset::PbrMaterialParams& params,
              engine::asset::MaterialTextures textures = {}) {
    return engine::asset::upload_material(params, std::move(textures), g.allocator, g.transfer);
}

struct Passes {
    r::MeshPass     mesh;
    r::HiZPass      hiz;
    r::ShadowPass   shadow;
    r::LightingPass lighting;
};

[[nodiscard]] inline std::expected<Passes, engine::core::Error> make_passes(GpuContext& g) {
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

// The camera on the sun's side (-x), looking down at the shadow.
[[nodiscard]] inline occlusion::Camera shadow_camera() {
    return occlusion::look_at({-6.0F, 4.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}, 60.0F, 1.0F, 0.1F,
                              60.0F);
}

// GBuffer (`gbuffer_draws`) -> Hi-Z -> cascades (`shadow_draws`: the pass
// routes opaque / masked / translucent casters by material) -> lighting.
[[nodiscard]] inline std::expected<HostImage, engine::core::Error>
render_lit(GpuContext& g, Passes& p, const occlusion::Camera& cam,
           std::span<const r::DrawItem> gbuffer_draws, std::span<const r::DrawItem> shadow_draws) {
    engine::rhi::TransientImagePool pool(g.device, g.allocator);
    engine::rhi::RenderGraph graph(pool);
    auto gbuffer = p.mesh.add_to_graph(graph, k_extent, cam.view_proj, gbuffer_draws);
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hiz = p.hiz.add_to_graph(graph, gbuffer->depth, k_extent);
    if (!hiz) return std::unexpected(hiz.error());
    r::CsmSettings csm;
    csm.max_distance = 40.0F;
    const r::CsmView view{cam.view, cam.proj, k_to_light};
    auto shadows = p.shadow.add_to_graph(graph, view, csm, &*hiz, shadow_draws);
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
    return render_to_host(g, graph, *hdr, k_extent, 8);
}

// The GBuffer albedo (RGBA8) of `draws` seen by `cam`.
[[nodiscard]] inline std::expected<HostImage, engine::core::Error>
render_albedo(GpuContext& g, Passes& p, const occlusion::Camera& cam,
              std::span<const r::DrawItem> draws) {
    engine::rhi::TransientImagePool pool(g.device, g.allocator);
    engine::rhi::RenderGraph graph(pool);
    auto gbuffer = p.mesh.add_to_graph(graph, k_extent, cam.view_proj, draws);
    if (!gbuffer) return std::unexpected(gbuffer.error());
    return render_to_host(g, graph, gbuffer->albedo, k_extent, 4);
}

// Mean lit luminance over a 3x3 pixel window around world point `p`.
[[nodiscard]] inline glm::vec3 mean_rgb(const HostImage& img, const occlusion::Camera& cam,
                                        const glm::vec3& p) {
    const glm::uvec2 px = occlusion::project(cam.view_proj, p, k_extent);
    glm::vec3 sum(0.0F);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto x = static_cast<std::uint32_t>(
                std::clamp(static_cast<int>(px.x) + dx, 0, static_cast<int>(k_extent.width) - 1));
            const auto y = static_cast<std::uint32_t>(
                std::clamp(static_cast<int>(px.y) + dy, 0, static_cast<int>(k_extent.height) - 1));
            sum += glm::vec3(img.rgba16f(x, y));
        }
    }
    return sum / 9.0F;
}

[[nodiscard]] inline float luma(const glm::vec3& c) {
    return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
}

} // namespace quat::test::shadow_scene
