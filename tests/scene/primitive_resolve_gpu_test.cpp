// resolve_primitives(): Primitive -> MeshRenderer with shared, uploaded assets.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/primitives.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;

TEST(GpuPrimitives, ResolveBuildsSharedMeshesAndMaterials) {
    QUAT_REQUIRE_GPU(ctx);
    entt::registry r;
    a::AssetManager assets;

    const entt::entity red1 = r.create();
    const entt::entity red2 = r.create();
    const entt::entity glass = r.create();
    const entt::entity floor = r.create();
    r.emplace<s::Primitive>(red1, s::PrimitiveShape::cube, glm::vec4(1, 0, 0, 1));
    r.emplace<s::Primitive>(red2, s::PrimitiveShape::cube, glm::vec4(1, 0, 0, 1));
    r.emplace<s::Primitive>(glass, s::PrimitiveShape::sphere, glm::vec4(0.6F, 0.8F, 1.0F, 0.3F));
    r.emplace<s::Primitive>(floor, s::PrimitiveShape::plane);
    ASSERT_TRUE(s::primitives_need_resolve(r));

    auto resolved = s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets);
    ASSERT_OK(resolved);
    EXPECT_EQ(*resolved, 4U);
    EXPECT_FALSE(s::primitives_need_resolve(r));

    for (const entt::entity e : {red1, red2, glass, floor}) {
        const auto& mr = r.get<s::MeshRenderer>(e);
        ASSERT_TRUE(mr.mesh.is_loaded());
        ASSERT_TRUE(mr.material.is_loaded());
        EXPECT_TRUE(mr.mesh->valid());
        EXPECT_TRUE(mr.material->valid());
    }
    // Same shape + material -> the very same assets.
    EXPECT_EQ(r.get<s::MeshRenderer>(red1).mesh.id(), r.get<s::MeshRenderer>(red2).mesh.id());
    EXPECT_EQ(r.get<s::MeshRenderer>(red1).material.id(),
              r.get<s::MeshRenderer>(red2).material.id());
    EXPECT_EQ(assets.count<a::MeshAsset>(), 3U); // cube, sphere, plane
    EXPECT_EQ(assets.count<a::MaterialAsset>(), 3U);

    const auto& glass_mat = *r.get<s::MeshRenderer>(glass).material;
    EXPECT_NE(glass_mat.params.flags & a::material_blend, 0U);
    EXPECT_FLOAT_EQ(glass_mat.params.base_color_factor.a, 0.3F);
    EXPECT_NE((*r.get<s::MeshRenderer>(floor).material).params.flags & a::material_double_sided, 0U);
    EXPECT_EQ(r.get<s::MeshRenderer>(red1).mesh->index_count, 36U);

    // Editing one primitive rebuilds only that entity, with a new material.
    const a::AssetId old_material = r.get<s::MeshRenderer>(red2).material.id();
    r.get<s::Primitive>(red2).color = {0, 1, 0, 1};
    r.get<s::Primitive>(red2).shape = s::PrimitiveShape::capsule;
    resolved = s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets);
    ASSERT_OK(resolved);
    EXPECT_EQ(*resolved, 1U);
    EXPECT_NE(r.get<s::MeshRenderer>(red2).material.id(), old_material);
    EXPECT_NE(r.get<s::MeshRenderer>(red2).mesh.id(), r.get<s::MeshRenderer>(red1).mesh.id());

    // Nothing stale: a no-op.
    resolved = s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets);
    ASSERT_OK(resolved);
    EXPECT_EQ(*resolved, 0U);
}

} // namespace
