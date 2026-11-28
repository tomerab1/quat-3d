// MaterialOverrideStore (A8): each entity with overrides gets its own derived
// material — a copy of the base with the overridden parameters — drawn
// instead of the base; replaced and orphaned materials are retired and freed
// only after the frames in flight, so live materials stay bounded.

#include <cstdint>
#include <expected>
#include <filesystem>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/primitives.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;

constexpr std::uint32_t k_frames_in_flight = 2;

const s::MaterialFieldInfo& field(std::string_view name) { return *s::find_material_field(name); }

TEST(GpuMaterialOverride, EachEntityDrawsItsOwnDerivedMaterial) {
    QUAT_REQUIRE_GPU(ctx);
    entt::registry r;
    a::AssetManager assets;
    s::MaterialOverrideStore store(k_frames_in_flight);
    const entt::entity one = r.create();
    const entt::entity two = r.create();
    const entt::entity plain = r.create();
    for (const entt::entity e : {one, two, plain}) {
        r.emplace<s::Primitive>(e, s::PrimitiveShape::sphere, glm::vec4(0.8F, 0.1F, 0.1F, 1.0F));
    }
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    const a::AssetId base = r.get<s::MeshRenderer>(one).material.id();
    const std::size_t cached_materials = assets.count<a::MaterialAsset>();

    s::set_entity_material_field(r, one, field("clearcoat"), glm::vec4(1.0F));
    s::set_entity_material_field(r, two, field("clearcoat"), glm::vec4(1.0F));
    ASSERT_TRUE(store.needs_resolve(r));
    auto rebuilt = store.resolve(r, ctx->allocator, ctx->transfer, 1);
    ASSERT_OK(rebuilt);
    EXPECT_EQ(*rebuilt, 2U);
    EXPECT_FALSE(store.needs_resolve(r));
    EXPECT_EQ(store.live_count(), 2U);

    const a::MaterialAsset* drawn = store.drawn(r, one, r.get<s::MeshRenderer>(one));
    ASSERT_NE(drawn, nullptr);
    EXPECT_TRUE(drawn->valid());
    EXPECT_FLOAT_EQ(drawn->params.clearcoat_factor, 1.0F);
    EXPECT_FLOAT_EQ(drawn->params.base_color_factor.r, 0.8F); // the base shows through
    EXPECT_NE(drawn, store.drawn(r, two, r.get<s::MeshRenderer>(two))); // one per entity
    EXPECT_EQ(store.drawn(r, plain, r.get<s::MeshRenderer>(plain)),
              &*r.get<s::MeshRenderer>(plain).material);
    EXPECT_EQ(r.get<s::MeshRenderer>(one).material.id(), base); // MeshRenderer keeps the base
    EXPECT_EQ(assets.count<a::MaterialAsset>(), cached_materials)
        << "derived materials stay out of the asset manager";

    // Editing the primitive rebuilds its material, then the override on top.
    s::set_entity_material_field(r, one, field("roughness"), glm::vec4(0.1F));
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    EXPECT_EQ(store.drawn(r, one, r.get<s::MeshRenderer>(one)),
              &*r.get<s::MeshRenderer>(one).material)
        << "a stale derived material is never drawn";
    ASSERT_TRUE(store.needs_resolve(r));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, 2));
    drawn = store.drawn(r, one, r.get<s::MeshRenderer>(one));
    EXPECT_FLOAT_EQ(drawn->params.roughness_factor, 0.1F);
    EXPECT_FLOAT_EQ(drawn->params.clearcoat_factor, 1.0F);

    // Glass through an override: the transmission flag routes it to the
    // forward pass.
    s::set_entity_material_field(r, two, field("transmission"), glm::vec4(1.0F));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, 3));
    drawn = store.drawn(r, two, r.get<s::MeshRenderer>(two));
    EXPECT_NE(drawn->params.flags & a::material_transmission, 0U);
}

// Dragging a slider: every value rebuilds the entity's material, yet the
// materials alive at once never exceed the current one plus those retired
// within the frames in flight; removing the override frees them all.
TEST(GpuMaterialOverride, LiveMaterialsStayBoundedWhileTuning) {
    QUAT_REQUIRE_GPU(ctx);
    entt::registry r;
    a::AssetManager assets;
    s::MaterialOverrideStore store(k_frames_in_flight);
    const entt::entity e = r.create();
    r.emplace<s::Primitive>(e, s::PrimitiveShape::cube);
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));

    std::uint64_t frame = 1;
    for (int i = 0; i < 40; ++i, ++frame) {
        store.collect(frame);
        s::set_entity_material_field(r, e, field("clearcoat_roughness"),
                                     glm::vec4(static_cast<float>(i) / 40.0F));
        auto rebuilt = store.resolve(r, ctx->allocator, ctx->transfer, frame);
        ASSERT_OK(rebuilt);
        ASSERT_EQ(*rebuilt, 1U) << "value " << i;
        EXPECT_EQ(store.live_count(), 1U);
        EXPECT_LE(store.live_count() + store.retired_count(), 1U + k_frames_in_flight)
            << "value " << i;
    }

    // The override goes away: its material is retired, then freed.
    r.remove<s::MaterialOverride>(e);
    ASSERT_TRUE(store.needs_resolve(r));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, frame));
    EXPECT_EQ(store.live_count(), 0U);
    for (std::uint32_t i = 0; i <= k_frames_in_flight; ++i) store.collect(++frame);
    EXPECT_EQ(store.retired_count(), 0U);

    // A destroyed entity (or a replaced scene) orphans its material the same way.
    s::set_entity_material_field(r, e, field("clearcoat"), glm::vec4(0.5F));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, ++frame));
    EXPECT_EQ(store.live_count(), 1U);
    r.destroy(e);
    ASSERT_TRUE(store.needs_resolve(r));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, ++frame));
    EXPECT_EQ(store.live_count(), 0U);
    store.clear();
    EXPECT_EQ(store.retired_count(), 0U);
}

// The derived material is a copy of the base: every field but the parameter
// buffer carries over.
TEST(GpuMaterialOverride, DerivedMaterialKeepsTheBaseFields) {
    QUAT_REQUIRE_GPU(ctx);
    a::AssetManager assets;
    const auto texture = assets.load<a::TextureAsset>("test/texture", [](const auto&) {
        return std::expected<a::TextureAsset, engine::core::Error>(a::TextureAsset{});
    });
    a::MaterialTextures textures;
    textures.base_color = texture;
    textures.normal = texture;
    a::PbrMaterialParams params;
    params.flags = a::material_has_base_color | a::material_has_normal;
    auto base = a::upload_material(params, textures, ctx->allocator, ctx->transfer);
    ASSERT_OK(base);

    a::PbrMaterialParams tuned = base->params;
    tuned.roughness_factor = 0.2F;
    auto derived = a::derive_material(*base, tuned, ctx->allocator, ctx->transfer);
    ASSERT_OK(derived);
    EXPECT_TRUE(derived->valid());
    EXPECT_NE(derived->param_buffer.handle(), base->param_buffer.handle());
    EXPECT_FLOAT_EQ(derived->params.roughness_factor, 0.2F);
    EXPECT_EQ(derived->params.flags, base->params.flags);
    EXPECT_EQ(derived->textures.base_color.id(), base->textures.base_color.id());
    EXPECT_EQ(derived->textures.normal.id(), base->textures.normal.id());
    EXPECT_FALSE(derived->textures.emissive.valid());
}

// A6 x A8: a primitive drawn with an authored material shader keeps it (and
// its shader parameters) through a per-entity override, so the GBuffer pass
// still selects the authored pipeline for the derived material.
TEST(GpuMaterialOverride, TheDerivedMaterialKeepsTheMaterialShader) {
    QUAT_REQUIRE_GPU(ctx);
    entt::registry r;
    a::AssetManager assets;
    s::MaterialOverrideStore store(k_frames_in_flight);
    const entt::entity e = r.create();
    s::Primitive& p = r.emplace<s::Primitive>(e, s::PrimitiveShape::sphere);
    p.shader = "lava";
    p.shader_params[0] = glm::vec4(1.0F, 0.35F, 0.05F, 6.0F);
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    const a::MaterialAsset* base = &*r.get<s::MeshRenderer>(e).material;
    ASSERT_EQ(base->shader, "lava");

    s::set_entity_material_field(r, e, field("normal_scale"), glm::vec4(0.5F));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, 1));
    const a::MaterialAsset* drawn = store.drawn(r, e, r.get<s::MeshRenderer>(e));
    ASSERT_NE(drawn, nullptr);
    ASSERT_NE(drawn, base) << "the override is drawn";
    EXPECT_EQ(drawn->shader, "lava") << "the authored pipeline is still selected";
    EXPECT_EQ(drawn->params.shader_params[0], glm::vec4(1.0F, 0.35F, 0.05F, 6.0F));
    EXPECT_FLOAT_EQ(drawn->params.normal_scale, 0.5F);

    // Switching the primitive to another shader rebuilds the base, and the
    // override on top draws with the new shader.
    r.get<s::Primitive>(e).shader = "glow";
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    ASSERT_TRUE(store.needs_resolve(r));
    ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, 2));
    drawn = store.drawn(r, e, r.get<s::MeshRenderer>(e));
    EXPECT_EQ(drawn->shader, "glow");
    EXPECT_FLOAT_EQ(drawn->params.normal_scale, 0.5F);
}

} // namespace
