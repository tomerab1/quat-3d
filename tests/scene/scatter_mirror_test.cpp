// Scatter mirroring: a `mirror` fraction of the placements gets a negated
// local X scale; nothing else about a placement changes, the choice is
// deterministic per seed, and the field round-trips through the scene codec.

#include <cmath>
#include <cstddef>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scatter.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;

s::Scatter field(float mirror) {
    s::Scatter sc;
    sc.seed = 42;
    sc.size = {60.0F, 60.0F};
    sc.density = 0.5F;
    sc.min_spacing = 0.5F;
    sc.scale_range = {0.5F, 1.5F};
    sc.slope_deg = {0.0F, 90.0F};
    sc.mirror = mirror;
    return sc;
}

std::size_t mirrored(const std::vector<s::InstanceTransform>& placed) {
    std::size_t n = 0;
    for (const s::InstanceTransform& t : placed) n += t.scale.x < 0.0F ? 1U : 0U;
    return n;
}

const glm::mat4 k_identity(1.0F);

} // namespace

TEST(ScatterMirror, FractionOfPlacementsIsMirroredAndNothingElseChanges) {
    const auto plain = s::generate_scatter(field(0.0F), k_identity, {});
    const auto half = s::generate_scatter(field(0.5F), k_identity, {});
    const auto all = s::generate_scatter(field(1.0F), k_identity, {});
    ASSERT_GT(plain.size(), 500U);
    ASSERT_EQ(half.size(), plain.size());
    ASSERT_EQ(all.size(), plain.size());

    EXPECT_EQ(mirrored(plain), 0U);
    EXPECT_EQ(mirrored(all), all.size());
    const double share = static_cast<double>(mirrored(half)) / static_cast<double>(half.size());
    EXPECT_NEAR(share, 0.5, 0.08);

    for (std::size_t i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(half[i].position, plain[i].position);
        EXPECT_EQ(half[i].rotation, plain[i].rotation);
        EXPECT_EQ(std::abs(half[i].scale.x), plain[i].scale.x);
        EXPECT_EQ(half[i].scale.y, plain[i].scale.y);
        EXPECT_EQ(half[i].scale.z, plain[i].scale.z);
    }
    // Deterministic: the same seed mirrors the same placements.
    const auto again = s::generate_scatter(field(0.5F), k_identity, {});
    for (std::size_t i = 0; i < half.size(); ++i) EXPECT_EQ(again[i].scale.x, half[i].scale.x);
}

TEST(ScatterMirror, ChangingTheFractionRegenerates) {
    const glm::mat4 world(1.0F);
    EXPECT_NE(s::scatter_key(field(0.0F), world, 0), s::scatter_key(field(0.25F), world, 0));
}

TEST(ScatterMirror, CodecRoundTripsAndValidatesTheField) {
    entt::registry r;
    const entt::entity e = r.create();
    ASSERT_OK(s::apply_component(r, e, "scatter", {{"mirror", 0.3}}, /*strict=*/true));
    ASSERT_TRUE(r.all_of<s::Scatter>(e));
    EXPECT_FLOAT_EQ(r.get<s::Scatter>(e).mirror, 0.3F);
    nlohmann::json enc = s::encode_components(r, e);
    ASSERT_TRUE(enc["scatter"]["mirror"].is_number());
    EXPECT_NEAR(enc["scatter"]["mirror"].get<double>(), 0.3, 1e-6);
    EXPECT_FALSE(s::apply_component(r, e, "scatter", {{"mirror", 1.5}}, /*strict=*/true));
}
