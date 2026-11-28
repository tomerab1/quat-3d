#include <cstring>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/geometry/stone.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/dynamic_mesh.hpp"
#include "engine/scene/dynamic_mesh_gpu.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/render_readback.hpp"

namespace {

namespace s = engine::scene;
namespace geo = engine::geometry;
using quat::test::GpuContext;

// One frame of the loop: promote, record this frame's uploads, execute.
void run_frame(GpuContext& g, s::DynamicMeshStore& store, std::uint64_t frame) {
    s::dynamic_mesh_begin_frame(store, frame);
    engine::rhi::RenderGraph graph;
    ASSERT_OK(s::record_dynamic_mesh_uploads(store, g.allocator, graph, frame));
    ASSERT_OK(graph.compile());
    ASSERT_OK(quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) { graph.execute(cmd); }));
}

// The bytes of a device buffer.
std::vector<std::byte> read_buffer(GpuContext& g, const engine::rhi::GpuBuffer& buffer) {
    auto host = quat::test::host_readback_buffer(g.allocator, buffer.size());
    EXPECT_TRUE(host.has_value());
    if (!host) return {};
    EXPECT_TRUE(quat::test::submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
                    VkBufferCopy region{0, 0, buffer.size()};
                    vkCmdCopyBuffer(cmd, buffer.handle(), host->handle(), 1, &region);
                }).has_value());
    EXPECT_TRUE(host->invalidate_mapped().has_value());
    std::vector<std::byte> out(buffer.size());
    std::memcpy(out.data(), host->mapped(), out.size());
    return out;
}

template <typename T>
bool same_bytes(const std::vector<std::byte>& gpu, const std::vector<T>& cpu) {
    return gpu.size() == cpu.size() * sizeof(T) && std::memcmp(gpu.data(), cpu.data(), gpu.size()) == 0;
}

} // namespace

TEST(GpuDynamicMesh, UploadsWithinTheBudgetThenSwapsIn) {
    QUAT_REQUIRE_GPU(ctx);
    s::DynamicMeshStore::Config config;
    config.frame_byte_budget = 4096; // a ~45 KB stone takes a dozen frames
    s::DynamicMeshStore store(config);
    auto stone = geo::generate_stone({});
    ASSERT_OK(stone);
    const std::size_t bytes = stone->vertices.size() * sizeof(engine::asset::Vertex) +
                              stone->indices.size() * sizeof(std::uint32_t);
    ASSERT_OK(store.submit("rock", *stone));

    std::uint64_t frame = 1;
    std::uint64_t version = 0;
    std::size_t frames_uploading = 0;
    for (; frame < 200 && version == 0; ++frame) {
        run_frame(*ctx, store, frame);
        EXPECT_LE(store.stats().uploaded_bytes_last_frame, config.frame_byte_budget);
        frames_uploading += store.stats().uploaded_bytes_last_frame > 0 ? 1 : 0;
        (void)s::dynamic_mesh_resident(store, "rock", &version);
    }
    ASSERT_EQ(version, 1U);
    EXPECT_GE(frames_uploading, bytes / config.frame_byte_budget);
    const auto mesh = s::dynamic_mesh_resident(store, "rock");
    ASSERT_TRUE(mesh.is_loaded());
    EXPECT_TRUE(mesh->valid());
    EXPECT_EQ(mesh->index_count, stone->indices.size());
    EXPECT_TRUE(same_bytes(read_buffer(*ctx, mesh->vertex_buffer), stone->vertices));
    EXPECT_TRUE(same_bytes(read_buffer(*ctx, mesh->index_buffer), stone->indices));

    // A region update stages only the changed vertices; the rest is copied
    // on the GPU from the resident version, and that version keeps drawing
    // until the new one is in.
    std::vector<engine::asset::Vertex> moved(stone->vertices.begin() + 10, stone->vertices.begin() + 20);
    for (auto& v : moved) v.position += glm::vec3(0.0F, 1.0F, 0.0F);
    ASSERT_OK(store.update_vertices("rock", 10, moved));
    std::size_t staged = 0;
    std::uint64_t updated = 1;
    for (int i = 0; i < 5 && updated == 1; ++i, ++frame) {
        run_frame(*ctx, store, frame);
        staged += store.stats().uploaded_bytes_last_frame;
        (void)s::dynamic_mesh_resident(store, "rock", &updated);
    }
    ASSERT_EQ(updated, 2U);
    EXPECT_EQ(staged, moved.size() * sizeof(engine::asset::Vertex));
    const auto after = s::dynamic_mesh_resident(store, "rock");
    EXPECT_TRUE(same_bytes(read_buffer(*ctx, after->vertex_buffer), store.mesh("rock")->vertices));
    EXPECT_TRUE(same_bytes(read_buffer(*ctx, after->index_buffer), stone->indices));
    // The replaced version is still referenced here, so it is alive; the store
    // frees its own reference once the frames in flight are done.
    EXPECT_TRUE(mesh->valid());
    store.clear();
}

TEST(GpuDynamicMesh, ResolveBuildsTheMeshRendererFromAGenerator) {
    QUAT_REQUIRE_GPU(ctx);
    s::DynamicMeshStore store;
    engine::asset::AssetManager assets;
    entt::registry r;
    const entt::entity e = r.create();
    s::DynamicMesh d;
    d.mesh = "roof";
    d.generator = {{"type", "roof"}, {"polygon", {{0, 0}, {6, 0}, {6, 4}, {0, 4}}}, {"base_height", 3.0}};
    d.material.color = {0.7F, 0.3F, 0.2F, 1.0F};
    r.emplace<s::DynamicMesh>(e, d);

    ASSERT_TRUE(s::dynamic_meshes_need_resolve(r, store));
    ASSERT_OK(s::resolve_dynamic_meshes(r, store, ctx->allocator, ctx->transfer, assets));
    EXPECT_TRUE(store.contains("roof"));            // the generator ran
    EXPECT_FALSE(r.all_of<s::MeshRenderer>(e));    // not resident yet
    for (std::uint64_t frame = 1; frame < 4; ++frame) {
        run_frame(*ctx, store, frame);
    }
    s::dynamic_mesh_begin_frame(store, 4);
    ASSERT_TRUE(s::dynamic_meshes_need_resolve(r, store));
    ASSERT_OK(s::resolve_dynamic_meshes(r, store, ctx->allocator, ctx->transfer, assets));
    ASSERT_TRUE(r.all_of<s::MeshRenderer>(e));
    const auto& mr = r.get<s::MeshRenderer>(e);
    EXPECT_TRUE(mr.mesh->valid());
    EXPECT_FLOAT_EQ(mr.mesh->bounds.max.y, 5.0F); // 4 m wide at 45 degrees: 2 m above the eaves
    EXPECT_TRUE(mr.material.is_loaded());
    EXPECT_FALSE(s::dynamic_meshes_need_resolve(r, store));

    // Removing the mesh from the store removes what the entity draws.
    store.remove("roof");
    r.get<s::DynamicMesh>(e).generator = nullptr;
    ASSERT_TRUE(s::dynamic_meshes_need_resolve(r, store));
    ASSERT_OK(s::resolve_dynamic_meshes(r, store, ctx->allocator, ctx->transfer, assets));
    EXPECT_FALSE(r.all_of<s::MeshRenderer>(e));
    r.clear();
    store.clear();
}
