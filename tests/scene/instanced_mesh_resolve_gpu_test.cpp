// resolve_instanced_meshes() (R9.2): InstancedMesh -> InstancedMeshParts with
// uploaded, shared assets — primitive shapes and glTF node subtrees.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/primitives.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;

// A unit triangle mesh used by two nodes of a "Tree" (trunk at the origin,
// crown 2 m up, both under the Tree node 10 m along X) plus an unrelated
// "Rock" node.
constexpr const char* k_tree_gltf = R"({"asset":{"version":"2.0"},
 "buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
 "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
 "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
              {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
 "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],
 "nodes":[{"name":"Tree","translation":[10,0,0],"children":[1,2]},
          {"name":"Trunk","mesh":0},
          {"name":"Crown","mesh":0,"translation":[0,2,0]},
          {"name":"Rock","mesh":0,"translation":[-5,0,0]}],
 "scenes":[{"nodes":[0,3]}],"scene":0})";

TEST(GpuInstancedMesh, ResolvesPrimitiveAndGltfSources) {
    QUAT_REQUIRE_GPU(ctx);
    entt::registry r;
    a::AssetManager assets;

    const auto dir = std::filesystem::temp_directory_path() / "quat_instanced_resolve";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "tree.gltf") << k_tree_gltf;

    const entt::entity cones = r.create();
    r.emplace<s::InstancedMesh>(cones).primitive.shape = s::PrimitiveShape::cone;
    const entt::entity tree = r.create();
    auto& tm = r.emplace<s::InstancedMesh>(tree);
    tm.gltf_path = "tree.gltf"; // relative: resolves against the root below
    tm.gltf_node = "Tree";
    const entt::entity whole = r.create();
    r.emplace<s::InstancedMesh>(whole).gltf_path = "tree.gltf";
    const entt::entity missing = r.create();
    r.emplace<s::InstancedMesh>(missing).gltf_path = "no_such_file.gltf";
    ASSERT_TRUE(s::instanced_meshes_need_resolve(r));

    // The missing file is reported after everything else resolved.
    EXPECT_ERR(s::resolve_instanced_meshes(r, ctx->allocator, ctx->transfer, assets, dir));
    EXPECT_FALSE(s::instanced_meshes_need_resolve(r)); // failures are not retried unchanged

    const auto& cone_parts = r.get<s::InstancedMeshParts>(cones);
    ASSERT_EQ(cone_parts.parts.size(), 1U);
    EXPECT_TRUE(cone_parts.parts[0].mesh.is_loaded());
    EXPECT_TRUE(cone_parts.parts[0].material.is_loaded());
    EXPECT_GT(cone_parts.bounds.w, 0.5F); // a 1 m cone

    // Shares the Primitive component's assets.
    const s::PrimitiveAssets shared = s::load_primitive_assets(
        r.get<s::InstancedMesh>(cones).primitive, ctx->allocator, ctx->transfer, assets);
    EXPECT_EQ(shared.mesh.id(), cone_parts.parts[0].mesh.id());

    // The Tree subtree: trunk + crown relative to the Tree node.
    const auto& tree_parts = r.get<s::InstancedMeshParts>(tree);
    ASSERT_EQ(tree_parts.parts.size(), 2U);
    float lowest = 1e30F;
    float highest = -1e30F;
    for (const auto& p : tree_parts.parts) {
        EXPECT_NEAR(p.local[3].x, 0.0F, 1e-5F); // the node's own +10 m X is dropped
        lowest = std::min(lowest, p.local[3].y);
        highest = std::max(highest, p.local[3].y);
        EXPECT_TRUE(p.mesh.is_loaded());
    }
    EXPECT_NEAR(lowest, 0.0F, 1e-5F);
    EXPECT_NEAR(highest, 2.0F, 1e-5F);
    // Whole file: three mesh nodes in scene space.
    EXPECT_EQ(r.get<s::InstancedMeshParts>(whole).parts.size(), 3U);
    EXPECT_TRUE(r.get<s::InstancedMeshParts>(missing).parts.empty());

    // A changed source re-resolves only that entity; an unknown node fails.
    r.get<s::InstancedMesh>(tree).gltf_node = "Nope";
    EXPECT_ERR(s::resolve_instanced_meshes(r, ctx->allocator, ctx->transfer, assets, dir));
    EXPECT_TRUE(r.get<s::InstancedMeshParts>(tree).parts.empty());
    r.get<s::InstancedMesh>(tree).gltf_node = "Rock";
    auto again = s::resolve_instanced_meshes(r, ctx->allocator, ctx->transfer, assets, dir);
    ASSERT_OK(again);
    EXPECT_EQ(*again, 1U);
    EXPECT_EQ(r.get<s::InstancedMeshParts>(tree).parts.size(), 1U);

    std::filesystem::remove_all(dir);
}

} // namespace
