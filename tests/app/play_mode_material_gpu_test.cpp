// G7 x A8: Play/Stop (PlayMode's snapshot / restore) composes with the
// per-entity derived materials of the MaterialOverrideStore. Material edits
// made while playing are undone by Stop: the restored entities draw their
// base material until the store re-resolves them, never a stale derived
// material, and materials of entities that only existed during play are
// retired.

#include <cstdint>
#include <expected>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "app/play_mode.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/primitives.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;
namespace a = engine::asset;

const s::MaterialFieldInfo& field(std::string_view name) { return *s::find_material_field(name); }

TEST(GpuPlayModeMaterials, StopReResolvesRestoredOverrides) {
    QUAT_REQUIRE_GPU(ctx);
    s::Scene scene;
    entt::registry& r = scene.registry();
    a::AssetManager assets;
    s::MaterialOverrideStore store(2);
    std::uint64_t frame = 1;
    const auto settle = [&] {
        ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
        store.collect(frame);
        ASSERT_OK(store.resolve(r, ctx->allocator, ctx->transfer, frame));
        ++frame;
    };

    const entt::entity tuned = scene.create_entity("tuned");
    const entt::entity plain = scene.create_entity("plain");
    for (const entt::entity e : {tuned, plain}) {
        r.emplace<s::Primitive>(e, s::PrimitiveShape::sphere, glm::vec4(0.8F, 0.1F, 0.1F, 1.0F));
    }
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    s::set_entity_material_field(r, tuned, field("clearcoat"), glm::vec4(0.3F));
    settle();
    ASSERT_EQ(store.live_count(), 1U);

    quat::app::PlayMode mode;
    const quat::app::PlayMode::Hooks hooks{
        .begin = [](s::Scene&) -> std::expected<void, engine::core::Error> { return {}; },
        .end = [] {}};
    *mode.request() = true;
    ASSERT_TRUE(mode.apply(scene, hooks));

    // Tuning while playing: a changed override, a new one, a new entity.
    s::set_entity_material_field(r, tuned, field("clearcoat"), glm::vec4(0.9F));
    s::set_entity_material_field(r, plain, field("clearcoat"), glm::vec4(0.6F));
    const entt::entity spawned = scene.create_entity("spawned");
    r.emplace<s::Primitive>(spawned, s::PrimitiveShape::cube);
    ASSERT_OK(s::resolve_primitives(r, ctx->allocator, ctx->transfer, assets));
    s::set_entity_material_field(r, spawned, field("clearcoat"), glm::vec4(1.0F));
    settle();
    ASSERT_EQ(store.live_count(), 3U);
    EXPECT_FLOAT_EQ(store.drawn(r, tuned, r.get<s::MeshRenderer>(tuned))->params.clearcoat_factor, 0.9F);

    *mode.request() = false;
    ASSERT_TRUE(mode.apply(scene, hooks));
    ASSERT_FALSE(r.valid(spawned));

    // Before the store runs again, the restored entities draw their base
    // material — never the play session's derived one.
    const a::MaterialAsset* tuned_base = &*r.get<s::MeshRenderer>(tuned).material;
    const a::MaterialAsset* plain_base = &*r.get<s::MeshRenderer>(plain).material;
    EXPECT_EQ(store.drawn(r, tuned, r.get<s::MeshRenderer>(tuned)), tuned_base);
    EXPECT_EQ(store.drawn(r, plain, r.get<s::MeshRenderer>(plain)), plain_base);
    ASSERT_TRUE(store.needs_resolve(r));

    settle();
    const a::MaterialAsset* drawn = store.drawn(r, tuned, r.get<s::MeshRenderer>(tuned));
    ASSERT_NE(drawn, nullptr);
    EXPECT_NE(drawn, tuned_base);
    EXPECT_TRUE(drawn->valid());
    EXPECT_FLOAT_EQ(drawn->params.clearcoat_factor, 0.3F) << "the authored override is back";
    EXPECT_EQ(store.drawn(r, plain, r.get<s::MeshRenderer>(plain)), plain_base);
    EXPECT_EQ(store.live_count(), 1U) << "play-only materials were retired";
    EXPECT_FALSE(store.needs_resolve(r));
    for (int i = 0; i < 3; ++i) store.collect(frame++);
    EXPECT_EQ(store.retired_count(), 0U);
    store.clear();
}

} // namespace
