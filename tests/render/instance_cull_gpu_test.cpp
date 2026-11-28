// GPU instance culling (R6 / R9.4): the cull kernels match the CPU reference
// (instance_cull.hpp) view by view, and culled rendering matches unculled
// rendering pixel for pixel — the shadow cascades with GPU culling on and off,
// and both the cascades and the GBuffer on the direct-draw fallback.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/csm.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/instance_buffer.hpp"
#include "engine/renderer/instance_cull.hpp"
#include "engine/renderer/instance_cull_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/render_graph.hpp"
#include "render/occlusion_fixtures.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
namespace asset = engine::asset;
namespace occ = quat::test::occlusion;
using engine::core::Error;

const glm::vec3 k_eye(0.0F, 4.0F, 6.0F);
const glm::vec3 k_target(0.0F, 0.0F, -30.0F);
const glm::vec3 k_to_light = glm::normalize(glm::vec3(-0.5F, 1.0F, 0.3F));

// A unit box with bounds and, when `split`, two submeshes (two draws'
// worth of indirect commands).
std::expected<asset::MeshAsset, Error> unit_box(quat::test::GpuContext& g, bool split) {
    auto box = occ::upload_box(g, glm::vec3(-0.5F), glm::vec3(0.5F));
    if (!box) return std::unexpected(box.error());
    box->bounds = {glm::vec3(-0.5F), glm::vec3(0.5F)};
    if (split) {
        const std::uint32_t half = box->index_count / 2;
        box->submeshes = {asset::SubMesh{0, half, 0},
                          asset::SubMesh{half, box->index_count - half, 0}};
    }
    return box;
}

// A field of boxes around the camera and far beyond it, in two draws (the
// second split into two submeshes), with scales from pebbles to towers.
r::InstancedDrawList scatter(const asset::MeshAsset& a, const asset::MeshAsset& b,
                             const asset::MaterialAsset* material) {
    r::InstancedDrawList list;
    std::uint32_t        seed = 12345U;
    const auto rnd = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<float>(seed >> 8) / 16777216.0F;
    };
    for (int d = 0; d < 2; ++d) {
        r::InstancedDraw draw;
        draw.mesh = d == 0 ? &a : &b;
        draw.material = material;
        draw.first_instance = static_cast<std::uint32_t>(list.instances.size());
        for (int i = 0; i < 300; ++i) {
            const glm::vec3 p(rnd() * 240.0F - 120.0F, rnd() * 3.0F, rnd() * 240.0F - 180.0F);
            const float s = 0.05F + rnd() * rnd() * 4.0F;
            glm::mat4 m = glm::translate(glm::mat4(1.0F), p);
            m = glm::rotate(m, rnd() * 6.2831853F, glm::vec3(0.0F, 1.0F, 0.0F));
            m = glm::scale(m, glm::vec3(s, s * (0.5F + rnd() * 2.0F), s));
            list.instances.push_back(r::make_instance_data(m));
        }
        draw.instance_count = static_cast<std::uint32_t>(list.instances.size()) - draw.first_instance;
        list.draws.push_back(draw);
    }
    return list;
}

r::CsmView csm_view(const occ::Camera& cam) { return {cam.view, cam.proj, k_to_light}; }

r::CsmSettings cpu_fit_settings() {
    r::CsmSettings s;
    s.gpu_depth_bounds = false; // CPU-fitted cascades: the test knows them
    s.max_distance = 100.0F;
    s.caster_distance = 60.0F;
    return s;
}

// Copies `src` (a graph buffer) into a host buffer at the end of `graph`.
std::expected<rhi::GpuBuffer, Error> read_back(quat::test::GpuContext& g, rhi::RenderGraph& graph,
                                               rhi::ResourceHandle src, VkDeviceSize bytes,
                                               const char* name) {
    auto host = quat::test::host_readback_buffer(g.allocator, std::max<VkDeviceSize>(bytes, 4));
    if (!host) return std::unexpected(host.error());
    const VkBuffer dst = host->handle();
    (void)graph.add_pass(name, rhi::PassType::transfer)
        .reads(src, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            const VkBufferCopy region{0, 0, bytes};
            if (bytes > 0) vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(src).buffer, dst, 1, &region);
        });
    return host;
}

std::expected<void, Error> run(quat::test::GpuContext& g, rhi::RenderGraph& graph) {
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    return quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        quat::test::transfer_to_host_barrier(cmd);
    });
}

std::vector<std::uint32_t> host_u32(const rhi::GpuBuffer& b, std::size_t count) {
    std::vector<std::uint32_t> out(count);
    (void)b.invalidate_mapped();
    std::memcpy(out.data(), b.mapped(), count * sizeof(std::uint32_t));
    return out;
}

// Every layer of the cascaded shadow map, as raw depth texels.
std::expected<std::vector<std::uint8_t>, Error>
render_shadow_map(quat::test::GpuContext& g, r::ShadowPass& pass, const occ::Camera& cam,
                  const r::CsmSettings& settings, const r::InstancedDrawList& list) {
    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    auto shadows = pass.add_to_graph(graph, csm_view(cam), settings, nullptr, list);
    if (!shadows) return std::unexpected(shadows.error());
    const std::uint32_t texel = pass.shared().format() == VK_FORMAT_D16_UNORM ? 2 : 4;
    const VkDeviceSize layer = VkDeviceSize{r::shadow_map_size} * r::shadow_map_size * texel;
    const auto layers = static_cast<std::uint32_t>(settings.cascade_count);
    auto host = quat::test::host_readback_buffer(g.allocator, layer * layers);
    if (!host) return std::unexpected(host.error());
    const VkBuffer dst = host->handle();
    const rhi::ResourceHandle map = shadows->shadow_map;
    (void)graph.add_pass("test_shadow_readback", rhi::PassType::transfer)
        .reads(map, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            for (std::uint32_t c = 0; c < layers; ++c) {
                VkBufferImageCopy region{};
                region.bufferOffset = layer * c;
                region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, c, 1};
                region.imageExtent = {r::shadow_map_size, r::shadow_map_size, 1};
                vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(map).image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
            }
        });
    if (auto done = run(g, graph); !done) return std::unexpected(done.error());
    if (auto inv = host->invalidate_mapped(); !inv) return std::unexpected(inv.error());
    std::vector<std::uint8_t> out(static_cast<std::size_t>(layer * layers));
    std::memcpy(out.data(), host->mapped(), out.size());
    return out;
}

std::size_t differing(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) return std::max(a.size(), b.size());
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) n += a[i] != b[i] ? 1 : 0;
    return n;
}

// Restores the device's indirect-draw features when a test ends.
struct SimulateNoIndirect {
    rhi::Device& device;
    explicit SimulateNoIndirect(rhi::Device& d) : device(d) { device.simulate_missing_indirect(true); }
    ~SimulateNoIndirect() { device.simulate_missing_indirect(false); }
    SimulateNoIndirect(const SimulateNoIndirect&) = delete;
    SimulateNoIndirect& operator=(const SimulateNoIndirect&) = delete;
};

} // namespace

TEST(GpuInstanceCull, MatchesTheCpuReferenceInEveryView) {
    QUAT_REQUIRE_GPU(g);
    if (!g->device.indirect_caps().first_instance) GTEST_SKIP() << "no drawIndirectFirstInstance";
    auto a = unit_box(*g, false);
    ASSERT_OK(a);
    auto b = unit_box(*g, true);
    ASSERT_OK(b);
    const r::InstancedDrawList list = scatter(*a, *b, nullptr);
    const occ::Camera cam = occ::look_at(k_eye, k_target, 60.0F, 1.0F, 0.1F, 150.0F);
    const r::GpuCascades cascades =
        r::fit_cascades(csm_view(cam), cpu_fit_settings(), {0.1F, 100.0F}, r::shadow_map_size,
                        g->device.depth_clamp());
    auto cascade_buffer = g->allocator.create_buffer(
        sizeof(cascades),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(cascade_buffer);
    std::memcpy(cascade_buffer->mapped(), &cascades, sizeof(cascades));
    ASSERT_OK(cascade_buffer->flush());

    // Camera, two cascades (one with the projected-size cull), keep-all;
    // the second draw only casts within 40 m of the camera.
    r::InstancedDrawList culled = list;
    culled.draws[1].shadow_distance = 40.0F;
    const std::array<r::CullViewGpu, 4> views{r::camera_cull_view(cam.view_proj),
                                              r::cascade_cull_view(0, 0.0F),
                                              r::cascade_cull_view(3, 2.0F),
                                              r::keep_all_cull_view()};
    r::InstanceBuffer instances;
    auto address = instances.upload(g->device, g->allocator, culled.instances);
    ASSERT_OK(address);
    auto culler = r::InstanceCuller::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(culler);
    ASSERT_TRUE(culler->indirect());

    rhi::TransientImagePool pool(g->device, g->allocator);
    rhi::RenderGraph graph(pool);
    r::InstanceCullInput in;
    in.instances = *address;
    in.views = views;
    in.camera = cam.position;
    in.cascades = graph.import_buffer("test_cascades", cascade_buffer->handle(), sizeof(cascades));
    in.cascades_address = rhi::buffer_device_address(g->device.handle(), *cascade_buffer);
    ASSERT_OK(culler->add_to_graph(graph, culled, in));
    const r::InstanceCuller::Outputs out = culler->outputs();
    ASSERT_TRUE(out.visible.valid());
    const std::size_t n = culled.instances.size();
    const std::size_t commands = culler->command_count();
    ASSERT_EQ(commands, 3U); // one + two submeshes
    auto visible = read_back(*g, graph, out.visible, views.size() * n * 4, "test_visible");
    ASSERT_OK(visible);
    auto cmds = read_back(*g, graph, out.commands, views.size() * commands * 20, "test_commands");
    ASSERT_OK(cmds);
    auto counts = read_back(*g, graph, out.draw_counts, views.size() * 2 * 4, "test_counts");
    ASSERT_OK(counts);
    ASSERT_OK(run(*g, graph));

    const std::vector<std::uint32_t> vis = host_u32(*visible, views.size() * n);
    const std::vector<std::uint32_t> cmd = host_u32(*cmds, views.size() * commands * 5);
    const std::vector<std::uint32_t> cnt = host_u32(*counts, views.size() * 2);

    std::vector<glm::mat4> models;
    for (const r::InstanceData& d : culled.instances) models.push_back(d.model);
    std::array<r::CullDrawGpu, 2> draws{};
    std::uint32_t first_command = 0;
    for (std::size_t d = 0; d < 2; ++d) {
        const r::InstancedDraw& src = culled.draws[d];
        draws[d].local_sphere = {0.0F, 0.0F, 0.0F, std::sqrt(3.0F) * 0.5F};
        draws[d].first_instance = src.first_instance;
        draws[d].instance_count = src.instance_count;
        draws[d].first_command = first_command;
        draws[d].command_count = static_cast<std::uint32_t>(src.mesh->submeshes.size());
        draws[d].shadow_distance = src.shadow_distance;
        first_command += draws[d].command_count;
    }

    std::size_t compared = 0;
    for (std::size_t v = 0; v < views.size(); ++v) {
        const r::ResolvedCullView rv = r::resolve_cull_view(views[v], cascades);
        for (std::size_t d = 0; d < 2; ++d) {
            const r::CullDrawGpu& dr = draws[d];
            // The GPU's survivors of this draw, in its range of the list.
            const std::uint32_t kept = cmd[(v * commands + dr.first_command) * 5 + 1];
            ASSERT_LE(kept, dr.instance_count);
            std::vector<std::uint32_t> gpu(vis.begin() + static_cast<std::ptrdiff_t>(v * n + dr.first_instance),
                                           vis.begin() + static_cast<std::ptrdiff_t>(v * n + dr.first_instance + kept));
            std::sort(gpu.begin(), gpu.end());
            ASSERT_EQ(std::adjacent_find(gpu.begin(), gpu.end()), gpu.end()) << "duplicate slot";
            // Every instance the reference decides with a margin (radius
            // +-0.1 %) must agree; ones on a boundary may go either way.
            for (std::uint32_t k = 0; k < dr.instance_count; ++k) {
                const std::uint32_t i = dr.first_instance + k;
                const glm::vec4 s = r::world_sphere(models[i], dr.local_sphere);
                const bool lo = r::cull_accepts(rv, dr, {glm::vec3(s), s.w * 0.999F}, cam.position);
                const bool hi = r::cull_accepts(rv, dr, {glm::vec3(s), s.w * 1.001F}, cam.position);
                if (lo != hi) continue;
                EXPECT_EQ(std::binary_search(gpu.begin(), gpu.end(), i), lo)
                    << "view " << v << " draw " << d << " instance " << i;
                ++compared;
            }
            // Each submesh's command: its index range, the survivors, the
            // range start; the draw count skips empty draws.
            for (std::uint32_t c = 0; c < dr.command_count; ++c) {
                const std::size_t at = (v * commands + dr.first_command + c) * 5;
                const asset::SubMesh& sm = culled.draws[d].mesh->submeshes[c];
                EXPECT_EQ(cmd[at + 0], sm.index_count);
                EXPECT_EQ(cmd[at + 1], kept);
                EXPECT_EQ(cmd[at + 2], sm.index_offset);
                EXPECT_EQ(cmd[at + 3], 0U);
                EXPECT_EQ(cmd[at + 4], dr.first_instance);
            }
            EXPECT_EQ(cnt[v * 2 + d], kept > 0 ? dr.command_count : 0U);
        }
    }
    EXPECT_GT(compared, views.size() * n * 9 / 10);
    // The views really cull: the camera keeps a fraction, keep-all all.
    const auto kept_in = [&](std::size_t v) {
        return cmd[(v * commands) * 5 + 1] + cmd[(v * commands + 1) * 5 + 1];
    };
    EXPECT_LT(kept_in(0), n / 2);
    EXPECT_LT(kept_in(1), n / 2);
    EXPECT_EQ(kept_in(3), n);
}

TEST(GpuInstanceCull, CulledCascadesMatchUnculledOnes) {
    QUAT_REQUIRE_GPU(g);
    auto a = unit_box(*g, false);
    ASSERT_OK(a);
    auto b = unit_box(*g, true);
    ASSERT_OK(b);
    const r::InstancedDrawList list = scatter(*a, *b, nullptr);
    const occ::Camera cam = occ::look_at(k_eye, k_target, 60.0F, 1.0F, 0.1F, 150.0F);
    auto pass = r::ShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(pass);

    r::CsmSettings off = cpu_fit_settings();
    off.gpu_culling = false;
    auto reference = render_shadow_map(*g, *pass, cam, off, list);
    ASSERT_OK(reference);
    // The casters land in the map (depth below the clear value 1).
    const std::size_t cleared = differing(*reference, std::vector<std::uint8_t>(
                                                          reference->size(), 0xFF));
    ASSERT_GT(cleared, reference->size() / 100);
    // Culling alone (no projected-size cull) draws exactly the same depths.
    r::CsmSettings on = cpu_fit_settings();
    on.caster_min_texels = 0.0F;
    auto culled = render_shadow_map(*g, *pass, cam, on, list);
    ASSERT_OK(culled);
    EXPECT_EQ(differing(*reference, *culled), 0U);

    // The projected-size cull drops only small casters from coarse
    // cascades: a few texels change, most do not.
    on.caster_min_texels = 4.0F;
    auto small = render_shadow_map(*g, *pass, cam, on, list);
    ASSERT_OK(small);
    const std::size_t changed = differing(*reference, *small);
    EXPECT_LT(changed, reference->size() / 20);
}

TEST(GpuInstanceCull, DirectFallbackDrawsTheSameImages) {
    QUAT_REQUIRE_GPU(g);
    auto a = unit_box(*g, false);
    ASSERT_OK(a);
    auto b = unit_box(*g, true);
    ASSERT_OK(b);
    auto grey = occ::upload_grey(*g);
    ASSERT_OK(grey);
    const r::InstancedDrawList list = scatter(*a, *b, &*grey);
    const occ::Camera cam = occ::look_at(k_eye, k_target, 60.0F, 1.0F, 0.1F, 150.0F);
    constexpr VkExtent2D extent{256, 256};
    const r::CsmSettings csm = [] {
        r::CsmSettings s = cpu_fit_settings();
        s.caster_min_texels = 0.0F;
        return s;
    }();

    const auto gbuffer_albedo = [&](r::MeshPass& mesh) -> std::expected<quat::test::HostImage, Error> {
        rhi::TransientImagePool pool(g->device, g->allocator);
        rhi::RenderGraph graph(pool);
        auto targets = mesh.add_to_graph(graph, extent, cam.view_proj, list);
        if (!targets) return std::unexpected(targets.error());
        return quat::test::render_to_host(*g, graph, targets->albedo, extent, 4);
    };

    auto mesh = r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(mesh);
    auto shadow = r::ShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(shadow);
    auto culled_albedo = gbuffer_albedo(*mesh);
    ASSERT_OK(culled_albedo);
    auto culled_map = render_shadow_map(*g, *shadow, cam, csm, list);
    ASSERT_OK(culled_map);

    const SimulateNoIndirect no_indirect(g->device);
    auto direct_mesh =
        r::MeshPass::create(g->device, g->allocator, g->cache, g->transfer, g->shader_dir);
    ASSERT_OK(direct_mesh);
    auto direct_shadow = r::ShadowPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(direct_shadow);
    auto direct_albedo = gbuffer_albedo(*direct_mesh);
    ASSERT_OK(direct_albedo);
    auto direct_map = render_shadow_map(*g, *direct_shadow, cam, csm, list);
    ASSERT_OK(direct_map);

    EXPECT_EQ(differing(culled_albedo->bytes, direct_albedo->bytes), 0U);
    EXPECT_EQ(differing(*culled_map, *direct_map), 0U);
    // Something was drawn.
    std::size_t lit = 0;
    for (std::size_t i = 3; i < culled_albedo->bytes.size(); i += 4) {
        lit += culled_albedo->bytes[i] != 0 ? 1 : 0;
    }
    EXPECT_GT(lit, 1000U);
}
