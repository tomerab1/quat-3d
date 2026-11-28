#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/scene/dynamic_mesh.hpp"
#include "engine/scene/dynamic_mesh_gpu.hpp"
#include "scene/dynamic_mesh_store_impl.hpp"
#include "support/expect_ok.hpp"

namespace s = engine::scene;
namespace geo = engine::geometry;

namespace {

geo::MeshData triangle(float z = 0.0F) {
    geo::MeshData m;
    m.vertices.resize(3);
    m.vertices[0].position = {0, 0, z};
    m.vertices[1].position = {1, 0, z};
    m.vertices[2].position = {0, 1, z};
    m.indices = {0, 1, 2};
    return m;
}

} // namespace

TEST(DynamicMeshStore, SubmitValidatesAndVersions) {
    s::DynamicMeshStore store;
    EXPECT_ERR(store.submit("", triangle()));
    EXPECT_ERR(store.submit("has space", triangle()));
    EXPECT_ERR(store.submit(std::string(129, 'a'), triangle()));
    geo::MeshData broken = triangle();
    broken.indices = {0, 1, 7};
    EXPECT_ERR(store.submit("a", std::move(broken)));
    EXPECT_FALSE(store.contains("a"));

    auto v1 = store.submit("wall/segment-1", triangle());
    ASSERT_OK(v1);
    EXPECT_EQ(*v1, 1U);
    auto v2 = store.submit("wall/segment-1", triangle(1.0F));
    ASSERT_OK(v2);
    EXPECT_EQ(*v2, 2U);
    const auto info = store.info("wall/segment-1");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->version, 2U);
    EXPECT_EQ(info->resident_version, 0U); // nothing uploaded yet
    EXPECT_EQ(info->triangles, 1U);
    EXPECT_FLOAT_EQ(info->bounds.min.z, 1.0F);
    // One upload per id: the newer version superseded the older one.
    EXPECT_EQ(store.stats().pending_uploads, 1U);
    EXPECT_EQ(store.list().size(), 1U);
}

TEST(DynamicMeshStore, RefusesMeshesOverTheSizeLimit) {
    s::DynamicMeshStore::Config config;
    config.max_mesh_bytes = 64;
    s::DynamicMeshStore store(config);
    auto r = store.submit("big", triangle());
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("byte limit"), std::string::npos);
}

TEST(DynamicMeshStore, UpdateVerticesChecksTheRange) {
    s::DynamicMeshStore store;
    ASSERT_OK(store.submit("t", triangle()));
    std::vector<engine::asset::Vertex> two(2);
    EXPECT_ERR(store.update_vertices("missing", 0, two));
    EXPECT_ERR(store.update_vertices("t", 2, two)); // [2, 4) outside 3 vertices
    two[0].position = {5, 5, 5};
    auto v = store.update_vertices("t", 1, two);
    ASSERT_OK(v);
    EXPECT_EQ(*v, 2U);
    EXPECT_EQ(store.mesh("t")->vertices[1].position, glm::vec3(5, 5, 5));
    // Nothing resident yet, so this is a full upload, not a region update.
    const auto& upload = store.impl().uploads.front();
    EXPECT_FALSE(upload.region);
}

TEST(DynamicMeshStore, RemoveAndClear) {
    s::DynamicMeshStore store;
    ASSERT_OK(store.submit("a", triangle()));
    ASSERT_OK(store.submit("b", triangle()));
    EXPECT_TRUE(store.remove("a"));
    EXPECT_FALSE(store.remove("a"));
    EXPECT_EQ(store.stats().pending_uploads, 1U);
    store.clear();
    EXPECT_EQ(store.stats().meshes, 0U);
    EXPECT_FALSE(s::dynamic_mesh_resident(store, "b").valid());
}
