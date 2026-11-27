// resolve_material_overrides() (A8): overrides upload a derived material that
// the renderer draws instead of the base, shared between equal overrides and
// rebuilt when the base (a primitive's material) changes.

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/primitives.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;

TEST(GpuMaterialOverride, ResolveUploadsSharedDerivedMaterials) {
    QUAT_REQUIRE_GPU(ctx);
    entt::registry r;
    a::AssetManager assets;
    const entt::entity one = r.create();
    const entt::entity two = r.create();
    const entt::entity plain = r.create();
    for (const entt::entity e : {one, two, plain}) {
        r.emplace<s::Primitive>(e, s::PrimitiveShape::sphere, glm::vec4(0.8F, 0.1F, 0.1F, 1.0F));
    }
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    const a::AssetId base = r.get<s::MeshRenderer>(one).material.id();

    const s::MaterialFieldInfo& clearcoat = *s::find_material_field("clearcoat");
    s::set_entity_material_field(r, one, clearcoat, glm::vec4(1.0F));
    s::set_entity_material_field(r, two, clearcoat, glm::vec4(1.0F));
    ASSERT_TRUE(s::material_overrides_need_resolve(r));
    auto rebuilt = s::resolve_material_overrides(r, ctx->allocator, ctx->transfer, assets);
    ASSERT_OK(rebuilt);
    EXPECT_EQ(*rebuilt, 2U);
    EXPECT_FALSE(s::material_overrides_need_resolve(r));

    const a::MaterialAsset* drawn = s::drawn_material(r, one, r.get<s::MeshRenderer>(one));
    ASSERT_NE(drawn, nullptr);
    EXPECT_TRUE(drawn->valid());
    EXPECT_FLOAT_EQ(drawn->params.clearcoat_factor, 1.0F);
    EXPECT_FLOAT_EQ(drawn->params.base_color_factor.r, 0.8F); // the base shows through
    EXPECT_EQ(drawn, s::drawn_material(r, two, r.get<s::MeshRenderer>(two))); // shared
    EXPECT_EQ(s::drawn_material(r, plain, r.get<s::MeshRenderer>(plain)),
              &*r.get<s::MeshRenderer>(plain).material);
    EXPECT_EQ(r.get<s::MeshRenderer>(one).material.id(), base); // MeshRenderer keeps the base

    // Editing the primitive rebuilds its material, then the override on top.
    s::set_entity_material_field(r, one, *s::find_material_field("roughness"), glm::vec4(0.1F));
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    ASSERT_TRUE(s::material_overrides_need_resolve(r));
    ASSERT_OK(s::resolve_material_overrides(r, ctx->allocator, ctx->transfer, assets));
    drawn = s::drawn_material(r, one, r.get<s::MeshRenderer>(one));
    EXPECT_FLOAT_EQ(drawn->params.roughness_factor, 0.1F);
    EXPECT_FLOAT_EQ(drawn->params.clearcoat_factor, 1.0F);

    // Glass through an override: the transmission flag routes it to the
    // forward pass.
    s::set_entity_material_field(r, two, *s::find_material_field("transmission"), glm::vec4(1.0F));
    ASSERT_OK(s::resolve_material_overrides(r, ctx->allocator, ctx->transfer, assets));
    drawn = s::drawn_material(r, two, r.get<s::MeshRenderer>(two));
    EXPECT_NE(drawn->params.flags & a::material_transmission, 0U);
}

} // namespace
