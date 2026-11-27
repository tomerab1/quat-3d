// InstancedMesh render collection (R9.2): one instanced draw per (mesh,
// material) across entities, per-instance frustum culling, pass routing.
// Device-free: the meshes and materials are plain CPU-side assets.

#include <cstdint>
#include <filesystem>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;
using engine::renderer::InstancedDrawList;

class InstancedMeshCollect : public ::testing::Test {
protected:
    a::AssetManager                 assets;
    a::AssetHandle<a::MeshAsset>     trunk = cube_mesh("test/trunk");
    a::AssetHandle<a::MeshAsset>     crown = cube_mesh("test/crown");
    a::AssetHandle<a::MaterialAsset> bark = material("test/bark", 0);
    a::AssetHandle<a::MaterialAsset> glass = material("test/glass", a::material_blend);
    entt::registry                   r;

    // Camera at the origin looking down -Z, 60 degree fov, 0.1..500 m.
    glm::mat4 view_proj = [] {
        glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 500.0F);
        proj[1][1] *= -1.0F;
        return proj * glm::lookAt(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F),
                                  glm::vec3(0.0F, 1.0F, 0.0F));
    }();

    a::AssetHandle<a::MeshAsset> cube_mesh(const char* key) {
        return assets.load<a::MeshAsset>(key, [](const std::filesystem::path&) {
            a::MeshAsset m;
            m.bounds = {glm::vec3(-0.5F), glm::vec3(0.5F)};
            return std::expected<a::MeshAsset, engine::core::Error>(std::move(m));
        });
    }
    a::AssetHandle<a::MaterialAsset> material(const char* key, std::uint32_t flags) {
        return assets.load<a::MaterialAsset>(key, [flags](const std::filesystem::path&) {
            a::MaterialAsset m;
            m.params.flags = flags;
            return std::expected<a::MaterialAsset, engine::core::Error>(std::move(m));
        });
    }

    // An entity at `world` with `n` instances 2 m apart along -Z starting at
    // `start` (instance-local), drawn with `parts`.
    entt::entity make_entity(const glm::mat4& world, int n, glm::vec3 start,
                             std::vector<s::InstancedMeshPart> parts) {
        const entt::entity e = r.create();
        r.emplace<s::Transform>(e).world = world;
        auto& mesh = r.emplace<s::InstancedMesh>(e);
        for (int i = 0; i < n; ++i) {
            s::InstanceTransform t;
            t.position = start + glm::vec3(0.0F, 0.0F, -2.0F * static_cast<float>(i));
            t.rotation = glm::angleAxis(0.3F * static_cast<float>(i), glm::vec3(0, 1, 0));
            t.scale = glm::vec3(1.0F + 0.1F * static_cast<float>(i));
            mesh.instances.push_back(t);
        }
        s::InstancedMeshParts p;
        p.parts = std::move(parts);
        p.bounds = s::instanced_parts_bounds(p.parts);
        p.resolved = s::instanced_mesh_source_hash(mesh);
        r.emplace<s::InstancedMeshParts>(e, std::move(p));
        return e;
    }

    s::InstancedMeshPart part(const a::AssetHandle<a::MeshAsset>& m,
                              const a::AssetHandle<a::MaterialAsset>& mat,
                              glm::mat4 local = glm::mat4(1.0F)) {
        return s::InstancedMeshPart{m, mat, local};
    }
};

void expect_mat_near(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 4; ++k) EXPECT_NEAR(a[c][k], b[c][k], 1e-4F) << c << "," << k;
    }
}

TEST_F(InstancedMeshCollect, OneDrawPerMeshMaterialWithAllInstances) {
    const glm::mat4 crown_local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 0.0F));
    const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, -1.0F, -5.0F));
    const entt::entity e =
        make_entity(world, 50, glm::vec3(0.0F), {part(trunk, bark), part(crown, bark, crown_local)});

    InstancedDrawList list;
    const std::size_t n = s::append_instanced_draws(r, view_proj, s::InstancedPass::opaque, list);
    EXPECT_EQ(n, 100U);
    ASSERT_EQ(list.draws.size(), 2U);
    EXPECT_EQ(list.draws[0].mesh, &*trunk);
    EXPECT_EQ(list.draws[1].mesh, &*crown);
    for (const auto& d : list.draws) {
        EXPECT_EQ(d.material, &*bark);
        EXPECT_EQ(d.instance_count, 50U);
    }
    EXPECT_EQ(list.draws[0].first_instance, 0U);
    EXPECT_EQ(list.draws[1].first_instance, 50U);
    ASSERT_EQ(list.instances.size(), 100U);

    const auto& mesh = r.get<s::InstancedMesh>(e);
    for (std::size_t i = 0; i < 50; ++i) {
        const glm::mat4 model = world * s::instance_matrix(mesh.instances[i]);
        expect_mat_near(list.instances[i].model, model);
        expect_mat_near(list.instances[50 + i].model, model * crown_local);
    }
}

TEST_F(InstancedMeshCollect, EntitiesSharingMeshAndMaterialShareADraw) {
    make_entity(glm::mat4(1.0F), 10, glm::vec3(0.0F, 0.0F, -5.0F), {part(trunk, bark)});
    make_entity(glm::mat4(1.0F), 7, glm::vec3(3.0F, 0.0F, -5.0F), {part(trunk, bark)});

    // Pre-existing batched content is kept; ranges continue after it.
    InstancedDrawList list;
    list.instances.resize(3);
    list.draws.push_back({&*crown, &*bark, VK_NULL_HANDLE, 0, 3});
    s::append_instanced_draws(r, view_proj, s::InstancedPass::opaque, list);
    ASSERT_EQ(list.draws.size(), 2U);
    EXPECT_EQ(list.draws[1].first_instance, 3U);
    EXPECT_EQ(list.draws[1].instance_count, 17U);
    EXPECT_EQ(list.instances.size(), 20U);
}

TEST_F(InstancedMeshCollect, CullsInstancesOutsideTheFrustum) {
    // 10 in front of the camera (z = -5 .. -23), 10 behind it (z = +40 .. +22).
    make_entity(glm::mat4(1.0F), 10, glm::vec3(0.0F, 0.0F, -5.0F), {part(trunk, bark)});
    make_entity(glm::mat4(1.0F), 10, glm::vec3(0.0F, 0.0F, 40.0F), {part(trunk, bark)});
    // Far off to the side (outside the 60 degree cone).
    make_entity(glm::mat4(1.0F), 5, glm::vec3(100.0F, 0.0F, -10.0F), {part(trunk, bark)});

    InstancedDrawList list;
    EXPECT_EQ(s::append_instanced_draws(r, view_proj, s::InstancedPass::opaque, list), 10U);
    for (const auto& inst : list.instances) EXPECT_LT(inst.model[3].z, 0.0F);

    // A sphere straddling the near plane / frustum side still counts.
    InstancedDrawList edge;
    r.clear();
    make_entity(glm::mat4(1.0F), 1, glm::vec3(0.0F, 0.0F, 0.3F), {part(trunk, bark)});
    EXPECT_EQ(s::append_instanced_draws(r, view_proj, s::InstancedPass::opaque, edge), 1U);
}

TEST_F(InstancedMeshCollect, PassRouting) {
    const entt::entity e = make_entity(glm::mat4(1.0F), 4, glm::vec3(0.0F, 0.0F, -5.0F),
                                       {part(trunk, bark), part(crown, glass)});

    InstancedDrawList opaque;
    s::append_instanced_draws(r, view_proj, s::InstancedPass::opaque, opaque);
    ASSERT_EQ(opaque.draws.size(), 1U); // the blended part is not in the GBuffer
    EXPECT_EQ(opaque.draws[0].material, &*bark);

    InstancedDrawList shadow;
    s::append_instanced_draws(r, view_proj, s::InstancedPass::shadow, shadow);
    EXPECT_EQ(shadow.instances.size(), 4U);

    std::vector<engine::renderer::DrawItem> transparent;
    EXPECT_EQ(s::append_instanced_transparent_items(r, view_proj, transparent), 4U);
    for (const auto& item : transparent) EXPECT_EQ(item.material, &*glass);

    r.get<s::InstancedMesh>(e).cast_shadows = false;
    InstancedDrawList none;
    EXPECT_EQ(s::append_instanced_draws(r, view_proj, s::InstancedPass::shadow, none), 0U);
    EXPECT_TRUE(none.draws.empty());
}

TEST_F(InstancedMeshCollect, PartsBoundsCoverEveryPart) {
    const glm::mat4 up = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 3.0F, 0.0F));
    const std::vector<s::InstancedMeshPart> parts{part(trunk, bark), part(crown, bark, up)};
    const glm::vec4 b = s::instanced_parts_bounds(parts);
    EXPECT_NEAR(b.x, 0.0F, 1e-5F);
    EXPECT_NEAR(b.y, 1.5F, 1e-5F);
    EXPECT_NEAR(b.z, 0.0F, 1e-5F);
    EXPECT_NEAR(b.w, glm::length(glm::vec3(1.0F, 4.0F, 1.0F)) * 0.5F, 1e-5F);
    EXPECT_EQ(s::instanced_parts_bounds({}), glm::vec4(0.0F));
}

TEST_F(InstancedMeshCollect, InstanceMatrixIsTrs) {
    s::InstanceTransform t;
    t.position = {1.0F, 2.0F, 3.0F};
    t.rotation = glm::angleAxis(0.7F, glm::normalize(glm::vec3(1.0F, 2.0F, 0.5F)));
    t.scale = {2.0F, 0.5F, 3.0F};
    const glm::mat4 expected = glm::translate(glm::mat4(1.0F), t.position) *
                               glm::mat4_cast(t.rotation) * glm::scale(glm::mat4(1.0F), t.scale);
    expect_mat_near(s::instance_matrix(t), expected);
}

TEST_F(InstancedMeshCollect, ResolveIsNeededWhenTheSourceChanges) {
    const entt::entity e = r.create();
    auto& mesh = r.emplace<s::InstancedMesh>(e);
    EXPECT_TRUE(s::instanced_meshes_need_resolve(r)); // no parts yet
    s::InstancedMeshParts parts;
    parts.resolved = s::instanced_mesh_source_hash(mesh);
    r.emplace<s::InstancedMeshParts>(e, parts);
    EXPECT_FALSE(s::instanced_meshes_need_resolve(r));

    // Placements are not part of the source.
    r.get<s::InstancedMesh>(e).instances.resize(10);
    EXPECT_FALSE(s::instanced_meshes_need_resolve(r));

    r.get<s::InstancedMesh>(e).primitive.color.r = 0.1F;
    EXPECT_TRUE(s::instanced_meshes_need_resolve(r));
    s::InstancedMesh gltf;
    gltf.gltf_path = "assets/meshes/tree.glb";
    s::InstancedMesh node = gltf;
    node.gltf_node = "Trunk";
    EXPECT_NE(s::instanced_mesh_source_hash(gltf), s::instanced_mesh_source_hash(node));
    EXPECT_NE(s::instanced_mesh_source_hash(gltf), s::instanced_mesh_source_hash(mesh));
}

} // namespace
