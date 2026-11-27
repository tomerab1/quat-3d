// Authored material shaders on the GPU (ROADMAP A6): a material whose `shader`
// names a compiled material module renders through that module's GBuffer
// program — its albedo and emission land in the GBuffer and the lighting pass
// adds the emission to the HDR image — and an edit recompiled by shader hot
// reload is swapped into the running pipeline. A shader that is not compiled
// falls back to the built-in program.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/material_shader_tree.hpp"

namespace {

namespace fs = std::filesystem;
namespace rhi = engine::rhi;
namespace r = engine::renderer;
namespace asset = engine::asset;

constexpr VkExtent2D k_extent{32, 32};

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Centre pixel of the GBuffer albedo (0..1), emission and lit HDR colour.
struct Sample {
    glm::vec3 albedo{0.0F};
    glm::vec3 emissive{0.0F};
    glm::vec3 hdr{0.0F};
};

// A clip-space triangle covering the centre of the target.
std::expected<asset::MeshAsset, engine::core::Error> make_triangle(quat::test::GpuContext& g) {
    std::vector<asset::Vertex> verts(3);
    verts[0].position = {-0.9F, -0.9F, 0.5F};
    verts[1].position = {0.9F, -0.9F, 0.5F};
    verts[2].position = {0.0F, 0.9F, 0.5F};
    for (asset::Vertex& v : verts) {
        v.normal = {0.0F, 0.0F, -1.0F};
        v.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
    }
    const std::vector<std::uint32_t> indices{0, 1, 2};
    auto vbuf = rhi::upload_device_buffer(g.allocator, g.transfer, verts.data(),
                                          verts.size() * sizeof(asset::Vertex),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vbuf) return std::unexpected(vbuf.error());
    auto ibuf = rhi::upload_device_buffer(g.allocator, g.transfer, indices.data(),
                                          indices.size() * sizeof(std::uint32_t),
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ibuf) return std::unexpected(ibuf.error());
    asset::MeshAsset mesh;
    mesh.vertex_buffer = std::move(*vbuf);
    mesh.index_buffer = std::move(*ibuf);
    mesh.vertex_count = 3;
    mesh.index_count = 3;
    mesh.submeshes = {asset::SubMesh{0, 3, 0}};
    return mesh;
}

// GBuffer (through `pass`) + lighting with no light and no ambient, so the HDR
// colour is exactly the emission; reads back the centre pixel.
std::expected<Sample, engine::core::Error> render(quat::test::GpuContext& g, r::MeshPass& pass,
                                                  r::LightingPass& lighting,
                                                  const asset::MeshAsset& mesh,
                                                  const asset::MaterialAsset& material) {
    const auto host = [&](VkDeviceSize bytes) {
        return g.allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    };
    auto albedo_rb = host(4);
    auto emissive_rb = host(8);
    auto hdr_rb = host(8);
    if (!albedo_rb || !emissive_rb || !hdr_rb) {
        return std::unexpected(engine::core::Error{"readback buffers"});
    }

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const r::DrawItem item{&mesh, &material, glm::mat4(1.0F)};
    auto gbuffer = pass.add_to_graph(graph, k_extent, glm::mat4(1.0F), {&item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    r::DirectionalLightParams dark;
    dark.color = glm::vec4(0.0F);
    dark.ambient = glm::vec4(0.0F);
    auto hdr = lighting.add_to_graph(graph, *gbuffer, k_extent, dark, glm::mat4(1.0F),
                                     glm::vec3(0.0F, 0.0F, -1.0F), rhi::ResourceHandle{},
                                     glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());

    const rhi::ResourceHandle a = gbuffer->albedo;
    const rhi::ResourceHandle e = gbuffer->emissive;
    const rhi::ResourceHandle h = *hdr;
    const VkBuffer a_dst = albedo_rb->handle();
    const VkBuffer e_dst = emissive_rb->handle();
    const VkBuffer h_dst = hdr_rb->handle();
    (void)graph.add_pass("readback", rhi::PassType::transfer)
        .reads(a, rhi::ResourceUsage::transfer_src)
        .reads(e, rhi::ResourceUsage::transfer_src)
        .reads(h, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {static_cast<std::int32_t>(k_extent.width / 2),
                                  static_cast<std::int32_t>(k_extent.height / 2), 0};
            region.imageExtent = {1, 1, 1};
            const auto copy = [&](rhi::ResourceHandle img, VkBuffer dst) {
                vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(img).image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
            };
            copy(a, a_dst);
            copy(e, e_dst);
            copy(h, h_dst);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        quat::test::transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());

    Sample s;
    const auto* px = static_cast<const std::uint8_t*>(albedo_rb->mapped());
    s.albedo = glm::vec3(px[0], px[1], px[2]) / 255.0F;
    const auto half3 = [](const void* data) {
        const auto* h16 = static_cast<const std::uint16_t*>(data);
        return glm::vec3(glm::unpackHalf1x16(h16[0]), glm::unpackHalf1x16(h16[1]),
                         glm::unpackHalf1x16(h16[2]));
    };
    s.emissive = half3(emissive_rb->mapped());
    s.hdr = half3(hdr_rb->mapped());
    return s;
}

void expect_near(const glm::vec3& got, const glm::vec3& want, float tol, const char* what) {
    EXPECT_NEAR(got.x, want.x, tol) << what;
    EXPECT_NEAR(got.y, want.y, tol) << what;
    EXPECT_NEAR(got.z, want.z, tol) << what;
}

TEST(GpuMaterialShader, RendersAnAuthoredMaterialAndSwapsInItsEdit) {
    QUAT_REQUIRE_GPU(g);
    if (!rhi::find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";
    quat::test::MaterialShaderTree tree("material_shader_gpu");
    ASSERT_TRUE(tree.ready()) << tree.error;

    // A private cache: the material programs and the GBuffer pass are
    // registered here, so the hot reload swaps exactly these pipelines.
    auto cache = rhi::PipelineCache::create(g->device);
    ASSERT_OK(cache);
    auto reload = rhi::ShaderHotReload::create(*cache, tree.reload_config());
    ASSERT_OK(reload);
    std::uint64_t frame = 1;
    const fs::path source = tree.materials / "glow.slang";
    const auto compile = [&](const std::string& text, std::uint32_t reloads) {
        write_file(source, text);
        EXPECT_TRUE((*reload)->request_recompile(source));
        for (int i = 0; i < 4000; ++i) {
            (*reload)->update(frame++);
            const rhi::ShaderStatus* st = (*reload)->status_of(source);
            if (st != nullptr && st->reloads >= reloads && (*reload)->idle()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const rhi::ShaderStatus* st = (*reload)->status_of(source);
        ASSERT_NE(st, nullptr);
        ASSERT_EQ(st->state, rhi::ShaderStatus::State::ok) << st->message;
        ASSERT_EQ(st->reloads, reloads);
    };
    compile(quat::test::test_material("flat_glow.slang"), 1);

    auto pass = r::MeshPass::create(g->device, g->allocator, *cache, g->transfer,
                                    tree.cooked.string());
    ASSERT_OK(pass);
    pass->set_material_time(0.0F);
    auto lighting = r::LightingPass::create(g->device, g->allocator, g->cache, g->shader_dir);
    ASSERT_OK(lighting);
    auto mesh = make_triangle(*g);
    ASSERT_OK(mesh);

    asset::PbrMaterialParams params;
    params.base_color_factor = {0.9F, 0.1F, 0.1F, 1.0F};
    params.shader_params[0] = {0.2F, 0.4F, 0.6F, 0.0F};
    params.shader_params[1] = {1.5F, 0.5F, 0.25F, 0.0F};
    auto material = asset::upload_material(params, {}, g->allocator, g->transfer);
    ASSERT_OK(material);
    material->shader = "glow";

    auto first = render(*g, *pass, *lighting, *mesh, *material);
    ASSERT_OK(first);
    expect_near(first->albedo, {0.2F, 0.4F, 0.6F}, 2.5F / 255.0F, "albedo from the shader");
    expect_near(first->emissive, {1.5F, 0.5F, 0.25F}, 1e-2F, "emission from the shader");
    expect_near(first->hdr, {1.5F, 0.5F, 0.25F}, 1e-2F, "lighting adds the emission");
    EXPECT_EQ(pass->material_pipelines().size(), 1U);

    // Edit: the hot reload recompiles and rebuilds the material's pipeline.
    compile(quat::test::test_material("flat_glow_swapped.slang"), 2);
    auto second = render(*g, *pass, *lighting, *mesh, *material);
    ASSERT_OK(second);
    expect_near(second->albedo, {1.0F, 0.5F, 0.25F}, 2.5F / 255.0F, "swapped albedo (clamped)");
    expect_near(second->emissive, {0.2F, 0.4F, 0.6F}, 1e-2F, "swapped emission");

    // A shader that is not compiled draws with the built-in program.
    material->shader = "not_compiled";
    auto fallback = render(*g, *pass, *lighting, *mesh, *material);
    ASSERT_OK(fallback);
    expect_near(fallback->albedo, {0.9F, 0.1F, 0.1F}, 2.5F / 255.0F, "built-in albedo");
    expect_near(fallback->emissive, {0.0F, 0.0F, 0.0F}, 1e-3F, "no emission");
    EXPECT_FALSE(pass->material_pipelines().error("not_compiled").empty());

    vkDeviceWaitIdle(g->device.handle());
    reload->reset();
}

} // namespace
