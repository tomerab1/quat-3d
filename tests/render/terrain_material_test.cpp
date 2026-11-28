// Layered terrain material on the GPU (R26.1): layer selection in the
// terrain GBuffer pass follows slope and altitude as the CPU reference of
// the blend rules says, and the per-tile attribute bake (curvature / flow)
// matches its CPU mirror.

#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/terrain_attributes.hpp"
#include "engine/renderer/terrain_material_gpu.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/terrain/heightmap.hpp"
#include "engine/terrain/material.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace t = engine::terrain;
using engine::core::Error;
using quat::test::HostImage;

constexpr std::uint32_t k_res = 129;
constexpr float k_size = 256.0F;
constexpr float k_snowline = 50.0F;

// Low meadow (x < 0.4), a 63-degree ramp, a high plateau above the snowline
// (x > 0.6).
float ground_height(float x01) {
    if (x01 < 0.4F) return 0.0F;
    if (x01 > 0.6F) return 100.0F;
    return (x01 - 0.4F) / 0.2F * 100.0F;
}

t::Heightmap ramp_map() {
    t::Heightmap m;
    m.resolution = k_res;
    m.tile_size_m = k_size;
    m.heights.resize(static_cast<std::size_t>(k_res) * k_res);
    for (std::uint32_t z = 0; z < k_res; ++z) {
        for (std::uint32_t x = 0; x < k_res; ++x) {
            m.heights[z * k_res + x] = ground_height(static_cast<float>(x) / (k_res - 1));
        }
    }
    m.min_height = 0.0F;
    m.max_height = 100.0F;
    return m;
}

// grass < rock on slopes < snow above the snowline; no noise, no macro
// variation, detail at every distance.
t::TerrainMaterial test_material() {
    t::TerrainMaterial m;
    m.layers.resize(3);
    m.layers[0].texture_set = "grass";
    m.layers[1].texture_set = "rock";
    m.layers[1].rule.slope_deg = {35.0F, 90.0F};
    m.layers[2].texture_set = "snow";
    m.layers[2].rule.altitude_m = {0.0F, 100000.0F};
    m.layers[2].rule.altitude_ref = t::AltitudeRef::snowline;
    m.layers[2].rule.slope_deg = {0.0F, 50.0F};
    for (auto& l : m.layers) l.rule.noise = 0.0F;
    m.macro_strength = 0.0F;
    m.far_start_m = 100000.0F;
    return m;
}

struct Render {
    HostImage albedo;
    glm::mat4 view_proj{1.0F};
};

const glm::vec3 k_eye(128.0F, 320.0F, 128.0F);
constexpr VkExtent2D k_extent{160, 160};

std::expected<Render, Error> render(quat::test::GpuContext& g) {
    auto mesh_pass = r::MeshPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto pass = r::TerrainPass::create(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
    if (!pass) return std::unexpected(pass.error());
    pass->set_material(test_material());
    pass->wait_for_textures(); // none cooked here: every layer is untextured
    if (auto set = pass->set_tile(glm::ivec2(0, 0), ramp_map(), g.transfer); !set) {
        return std::unexpected(set.error());
    }
    const glm::mat4 view =
        glm::lookAt(k_eye, glm::vec3(128.0F, 0.0F, 128.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 1000.0F);
    proj[1][1] *= -1.0F;
    Render out;
    out.view_proj = proj * view;
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto targets = mesh_pass->add_to_graph(graph, k_extent, out.view_proj, r::InstancedDrawList{});
    if (!targets) return std::unexpected(targets.error());
    pass->add_to_graph(graph, *targets, k_extent, out.view_proj, k_eye,
                       glm::vec3(128.0F, 0.0F, 128.0F), k_snowline);
    auto albedo = quat::test::render_to_host(g, graph, targets->albedo, k_extent, 4);
    if (!albedo) return std::unexpected(albedo.error());
    out.albedo = std::move(*albedo);
    return out;
}

const std::expected<Render, Error>& rendered(quat::test::GpuContext& g) {
    static const std::expected<Render, Error> image = render(g);
    return image;
}

// Linear albedo (the GBuffer target is UNORM) at the world point above
// tile fraction x01, z = 0.5.
glm::vec3 albedo_at(const Render& rd, float x01) {
    const float x = x01 * k_size; // tile (0,0) spans [0, 256] around anchor 128
    const glm::vec4 clip = rd.view_proj * glm::vec4(x, ground_height(x01), 128.0F, 1.0F);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    const auto px = static_cast<std::uint32_t>((ndc.x * 0.5F + 0.5F) * k_extent.width);
    const auto py = static_cast<std::uint32_t>((ndc.y * 0.5F + 0.5F) * k_extent.height);
    const glm::ivec4 c = rd.albedo.rgba8(px, py);
    return glm::vec3(c) / 255.0F;
}

// Index of the layer whose untextured colour is nearest to `c`.
int nearest_layer(const t::TerrainMaterial& m, glm::vec3 c) {
    int best = -1;
    float best_d = 1e9F;
    for (std::size_t i = 0; i < m.layers.size(); ++i) {
        const glm::vec3 ref = glm::vec3(r::terrain_set_fallback(m.layers[i].texture_set));
        const float d = glm::length(glm::normalize(c + 1e-4F) - glm::normalize(ref + 1e-4F)) +
                        std::abs(glm::length(c) - glm::length(ref));
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// The layer the CPU reference picks at tile fraction x01.
int reference_layer(float x01) {
    const float step = 1.0F / (k_res - 1);
    const float dh = ground_height(x01 + step) - ground_height(x01 - step);
    t::SurfacePoint p;
    p.slope_deg = glm::degrees(std::atan(std::abs(dh) / (2.0F * step * k_size)));
    p.height_m = ground_height(x01);
    p.snowline_m = k_snowline;
    p.noise = 0.5F;
    const t::LayerWeights w = t::layer_weights(test_material(), p);
    return static_cast<int>(std::max_element(w.begin(), w.end()) - w.begin());
}

} // namespace

TEST(GpuTerrainMaterial, LayerSelectionFollowsSlopeAndAltitude) {
    QUAT_REQUIRE_GPU(g);
    const auto& rd = rendered(*g);
    ASSERT_OK(rd);
    const t::TerrainMaterial m = test_material();
    struct Probe {
        float x01;
        int   layer;
    };
    for (const Probe p : {Probe{0.2F, 0}, Probe{0.5F, 1}, Probe{0.8F, 2}}) {
        EXPECT_EQ(reference_layer(p.x01), p.layer) << "x " << p.x01;
        const glm::vec3 c = albedo_at(*rd, p.x01); // linear (UNORM target)
        EXPECT_EQ(nearest_layer(m, c), p.layer)
            << "x " << p.x01 << " albedo " << c.r << " " << c.g << " " << c.b;
    }
}

TEST(GpuTerrainMaterial, AttributeBakeMatchesCpuReference) {
    QUAT_REQUIRE_GPU(g);
    // A cone-shaped valley with a ridge: flow gathers along the bottom.
    constexpr std::uint32_t res = 65;
    constexpr float mpt = 2.0F;
    std::vector<float> h(static_cast<std::size_t>(res) * res);
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const float fx = static_cast<float>(x) - 32.0F;
            const float fz = static_cast<float>(z);
            h[z * res + x] = std::abs(fx) * 0.8F + fz * 0.3F + 4.0F * std::sin(fz * 0.3F);
        }
    }
    auto image = rhi::upload_device_image(g->allocator, g->transfer, h.data(),
                                          h.size() * sizeof(float), {res, res},
                                          VK_FORMAT_R32_SFLOAT);
    ASSERT_OK(image);
    auto view = rhi::create_image_view(g->device.handle(), image->handle(), VK_FORMAT_R32_SFLOAT);
    ASSERT_OK(view);
    auto baker = r::TerrainAttributeBaker::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(baker);
    constexpr std::uint32_t sweeps = 24;
    auto target = baker->schedule(view->handle(), res, mpt, sweeps);
    ASSERT_OK(target);

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    graph.add_pass("bake", rhi::PassType::compute)
        .execute([&](rhi::PassContext& ctx) { baker->record(ctx.cmd()); });
    const rhi::ResourceHandle handle = graph.import_image(
        "attributes", target->image.handle(), target->view.handle(), VK_FORMAT_R8G8B8A8_UNORM,
        {res, res}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    auto out = quat::test::render_to_host(*g, graph, handle, {res, res}, 4);
    ASSERT_OK(out);

    const std::vector<glm::vec2> ref = r::terrain_attributes_reference(h, res, mpt, sweeps);
    float worst = 0.0F;
    for (std::uint32_t z = 0; z < res; ++z) {
        for (std::uint32_t x = 0; x < res; ++x) {
            const glm::ivec4 px = out->rgba8(x, z);
            const glm::vec2 gpu = glm::vec2(px.r, px.g) / 255.0F;
            const glm::vec2 cpu = ref[z * res + x];
            worst = std::max(worst, std::max(std::abs(gpu.x - cpu.x), std::abs(gpu.y - cpu.y)));
        }
    }
    EXPECT_LE(worst, 2.0F / 255.0F) << "GPU bake drifts from the CPU reference";
    // The valley floor collects water; the slopes stay dry; the floor is
    // concave (hollow), the flanks not.
    EXPECT_GT(ref[40 * res + 32].y, 0.6F);
    EXPECT_GT(ref[40 * res + 32].y, ref[40 * res + 8].y + 0.15F);
    EXPECT_GT(ref[40 * res + 32].x, 0.55F);
}
