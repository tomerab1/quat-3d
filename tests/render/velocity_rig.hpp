#pragma once

// Helpers for velocity-target tests (R11): a perspective camera, a unit quad,
// a GBuffer + background-velocity render that returns the RG16F target, and
// the CPU reference for a pixel on a posed plane.

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/velocity.hpp"
#include "engine/renderer/velocity_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace quat::test::velocity {

inline constexpr VkExtent2D k_extent{64, 64};

inline glm::mat4 perspective() {
    glm::mat4 p = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    p[1][1] *= -1.0F; // Vulkan y down
    return p;
}

// Looking at the origin from `eye`.
inline glm::mat4 camera(const glm::vec3& eye) {
    return perspective() * glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
}

inline glm::vec2 velocity_at(const HostImage& img, std::uint32_t x, std::uint32_t y) {
    std::uint16_t h[2];
    std::memcpy(h, img.at(x, y), sizeof(h));
    return {glm::unpackHalf1x16(h[0]), glm::unpackHalf1x16(h[1])};
}

// Unjittered NDC of the scene point pixel (x, y) shows (jitter.hpp).
inline glm::vec2 pixel_ndc(std::uint32_t x, std::uint32_t y, glm::vec2 jitter_ndc,
                           VkExtent2D extent = k_extent) {
    const glm::vec2 uv((static_cast<float>(x) + 0.5F) / static_cast<float>(extent.width),
                       (static_cast<float>(y) + 0.5F) / static_cast<float>(extent.height));
    return uv * 2.0F - 1.0F - jitter_ndc;
}

// Reference velocity of pixel (x, y) on a surface lying in its object's z = 0
// plane, posed by `model` now and `prev_model` last frame. The pixel's ray is
// intersected with the plane in object space; that object point is then
// projected through both frames.
inline glm::vec2 plane_reference(std::uint32_t x, std::uint32_t y, const glm::mat4& model,
                                 const glm::mat4& prev_model, const glm::mat4& vp,
                                 const glm::mat4& prev_vp,
                                 glm::vec2 jitter_ndc = glm::vec2(0.0F),
                                 VkExtent2D extent = k_extent) {
    const glm::mat4 inv = glm::inverse(vp * model);
    const glm::vec2 ndc = pixel_ndc(x, y, jitter_ndc, extent);
    glm::vec4 a = inv * glm::vec4(ndc, 0.0F, 1.0F);
    glm::vec4 b = inv * glm::vec4(ndc, 1.0F, 1.0F);
    a /= a.w;
    b /= b.w;
    const float t = a.z / (a.z - b.z);
    const glm::vec4 p(glm::vec3(a) + t * (glm::vec3(b) - glm::vec3(a)), 1.0F);
    return engine::renderer::surface_velocity(glm::vec3(model * p), glm::vec3(prev_model * p),
                                              vp, prev_vp);
}

// Pixel containing the projection of `world`.
inline glm::uvec2 pixel_of(const glm::vec3& world, const glm::mat4& vp,
                           VkExtent2D extent = k_extent) {
    const glm::vec2 uv = engine::renderer::project_to_uv(world, vp);
    return {static_cast<std::uint32_t>(uv.x * static_cast<float>(extent.width)),
            static_cast<std::uint32_t>(uv.y * static_cast<float>(extent.height))};
}

// A unit quad in the z = 0 plane, centred on the origin, facing +z.
inline std::expected<engine::asset::MeshAsset, engine::core::Error> quad(GpuContext& g) {
    std::array<engine::asset::Vertex, 4> v{};
    v[0].position = {-0.5F, -0.5F, 0.0F};
    v[1].position = {0.5F, -0.5F, 0.0F};
    v[2].position = {0.5F, 0.5F, 0.0F};
    v[3].position = {-0.5F, 0.5F, 0.0F};
    for (engine::asset::Vertex& vert : v) vert.normal = {0.0F, 0.0F, 1.0F};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    return upload_mesh(g, v, indices);
}

// Draws `list` into a GBuffer (rasterising with `raster_vp`), completes the
// background with VelocityPass (`vp` = unjittered view-projection) and returns
// the velocity target. `before` records into the command buffer first.
template <typename Before>
std::expected<HostImage, engine::core::Error>
render_velocity(GpuContext& g, const engine::renderer::InstancedDrawList& list,
                const glm::mat4& raster_vp, const glm::mat4& vp,
                const engine::renderer::ViewMotion& motion, Before&& before) {
    namespace r = engine::renderer;
    auto pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    auto background = r::VelocityPass::create(g.device, g.cache, g.shader_dir);
    if (!background) return std::unexpected(background.error());
    engine::rhi::TransientImagePool pool(g.device, g.allocator);
    engine::rhi::RenderGraph graph(pool);
    auto targets = pass->add_to_graph(graph, k_extent, raster_vp, list, motion);
    if (!targets) return std::unexpected(targets.error());
    background->add_background_to_graph(graph, *targets, k_extent, vp);
    return render_to_host(g, graph, targets->velocity, k_extent, 4, std::forward<Before>(before));
}

inline std::expected<HostImage, engine::core::Error>
render_velocity(GpuContext& g, const engine::renderer::InstancedDrawList& list,
                const glm::mat4& raster_vp, const glm::mat4& vp,
                const engine::renderer::ViewMotion& motion) {
    return render_velocity(g, list, raster_vp, vp, motion, [](VkCommandBuffer) {});
}

// Half-float storage plus the rasteriser's interpolation: an absolute floor
// and a small relative part.
inline void expect_velocity(const glm::vec2& got, const glm::vec2& want, const char* what) {
    const float tol = 2e-4F + 2e-3F * glm::length(want);
    EXPECT_NEAR(got.x, want.x, tol) << what;
    EXPECT_NEAR(got.y, want.y, tol) << what;
}

} // namespace quat::test::velocity
