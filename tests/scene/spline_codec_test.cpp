// The `spline` component codec and SplineCache upkeep (ROADMAP G18.1).

#include <optional>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/spline.hpp"
#include "scene/component_json_spline.hpp"

namespace quat::test {
namespace {

namespace s = engine::scene;
using nlohmann::json;

// Decodes like the codec's merge: into a copy, kept only on success.
[[nodiscard]] std::string decode(const json& v, s::Spline& out) {
    s::Spline copy = out;
    engine::core::JsonFields f(v, "spline");
    s::detail::decode_spline(f, copy);
    auto ok = f.finish();
    if (!ok) return ok.error().message;
    out = std::move(copy);
    return {};
}

TEST(SplineCodec, RoundTripsBothPointForms) {
    s::Spline in;
    in.kind = s::SplineKind::polyline;
    in.closed = true;
    in.follow_ground = false;
    in.ground_offset = 0.25F;
    in.network = 3;
    in.id = 17;
    in.points = {{{0, 0, 0}, 1.0F, 0.0F, 0}, {{4, 1, 0}, 2.5F, 3.0F, 6}, {{4, 0, 4}, 1.0F, 0.0F, 0}};
    const json j = s::detail::encode_spline(in);
    EXPECT_TRUE(j["points"][0].is_array()); // defaults: compact form
    EXPECT_TRUE(j["points"][1].is_object());
    s::Spline out;
    EXPECT_EQ(decode(j, out), "");
    EXPECT_EQ(out, in);
}

TEST(SplineCodec, MergesAndValidatesTheWhole) {
    s::Spline sp;
    ASSERT_EQ(decode(json::parse(R"({"points": [[0,0,0], [5,0,0]]})"), sp), "");
    EXPECT_EQ(sp.kind, s::SplineKind::centripetal_catmull_rom);
    // Closing a 2-point curve is impossible: the error names the field.
    EXPECT_EQ(decode(json::parse(R"({"closed": true})"), sp),
              "spline.points: a closed centripetal_catmull_rom spline needs at least 3 points, got 2");
    EXPECT_FALSE(sp.closed);
    EXPECT_EQ(decode(json::parse(R"({"points": [[0,0,0], {"position": [1,0,0], "width": -2}]})"), sp),
              "spline.points[1].width: expected a number in [0, 10000]");
    EXPECT_EQ(decode(json::parse(R"({"points": [[0,0,0], [1,0]]})"), sp),
              "spline.points[1]: expected [x, y, z] in metres (finite, within 1e6)");
    const std::string unknown = decode(json::parse(R"({"points": [[0,0,0], {"position": [1,0,0], "colour": 1}]})"), sp);
    EXPECT_EQ(unknown.rfind("spline.points[1]", 0), 0U) << unknown;
    EXPECT_NE(unknown.find("colour"), std::string::npos) << unknown;
    EXPECT_EQ(decode(json::parse(R"({"kind": "bspline"})"), sp).rfind("spline.kind", 0), 0U);
    EXPECT_EQ(decode(json::parse(R"({"kind": "cubic_bezier"})"), sp),
              "spline.points: a cubic_bezier spline needs at least 4 points, got 2");
}

TEST(SplineCache, BuildsInWorldSpaceDrapesAndTracksEdits) {
    entt::registry r;
    const entt::entity e = r.create();
    s::Transform t;
    t.world[3] = glm::vec4(100, 0, 0, 1);
    r.emplace<s::Transform>(e, t);
    s::Spline sp;
    sp.kind = s::SplineKind::polyline;
    sp.points = {{{0, 5, 0}, 2.0F, 1.0F, 0}, {{10, 5, 0}, 2.0F, 1.0F, 0}};
    sp.ground_offset = 0.1F;
    r.emplace<s::Spline>(e, sp);
    const engine::spline::GroundHeightFn ground = [](float x, float) -> std::optional<float> { return x * 0.01F; };

    EXPECT_EQ(s::update_spline_caches(r, ground, {.sample_spacing = 1.0F}), 1U);
    const auto& c = r.get<s::SplineCache>(e);
    ASSERT_TRUE(c.valid);
    ASSERT_EQ(c.samples.size(), 11U);
    EXPECT_EQ(c.samples[0].position.x, 100.0F);
    EXPECT_NEAR(c.samples[0].position.y, 1.1F, 1e-6F);
    EXPECT_NEAR(c.samples[10].position.y, 1.2F, 1e-6F);
    EXPECT_NEAR(c.bounds.max.y, 1.2F + 1.0F, 1e-6F); // draped top + wall height
    EXPECT_EQ(c.revision, 1U);

    // Unchanged: nothing rebuilt. Moved: rebuilt.
    EXPECT_EQ(s::update_spline_caches(r, ground, {.sample_spacing = 1.0F}), 0U);
    r.get<s::Spline>(e).points[1].position.x = 20.0F;
    EXPECT_EQ(s::update_spline_caches(r, ground, {.sample_spacing = 1.0F}), 1U);
    EXPECT_EQ(r.get<s::SplineCache>(e).samples.size(), 21U);

    // An invalid spline keeps a cache with the reason.
    r.get<s::Spline>(e).points.pop_back();
    s::update_spline_caches(r, ground);
    EXPECT_FALSE(r.get<s::SplineCache>(e).valid);
    EXPECT_EQ(r.get<s::SplineCache>(e).error, "points: a polyline spline needs at least 2 points, got 1");

    // Removing the spline drops its cache.
    r.remove<s::Spline>(e);
    s::update_spline_caches(r, ground);
    EXPECT_FALSE(r.all_of<s::SplineCache>(e));
}

TEST(SplineCache, RetriesSamplesOverUnloadedGround) {
    entt::registry r;
    const entt::entity e = r.create();
    s::Spline sp;
    sp.kind = s::SplineKind::polyline;
    sp.points = {{{0, 0, 0}, 1, 0, 0}, {{4, 0, 0}, 1, 0, 0}};
    r.emplace<s::Spline>(e, sp);
    bool loaded = false;
    const engine::spline::GroundHeightFn ground = [&loaded](float, float) -> std::optional<float> {
        if (!loaded) return std::nullopt;
        return 2.0F;
    };
    s::update_spline_caches(r, ground);
    EXPECT_GT(r.get<s::SplineCache>(e).missing_ground, 0U);
    loaded = true;
    std::size_t rebuilt = 0;
    for (int i = 0; i < 40 && rebuilt == 0; ++i) rebuilt = s::update_spline_caches(r, ground);
    EXPECT_EQ(rebuilt, 1U);
    EXPECT_EQ(r.get<s::SplineCache>(e).missing_ground, 0U);
    EXPECT_EQ(r.get<s::SplineCache>(e).samples[0].position.y, 2.0F);
}

} // namespace
} // namespace quat::test

namespace quat::test {
namespace {

TEST(SplineSceneNetwork, JoinsTheSplinesOfOneNetworkInWorldSpace) {
    entt::registry r;
    auto make = [&r](std::uint32_t network, std::uint32_t id, glm::vec3 a, glm::vec3 b, float dx) {
        const entt::entity e = r.create();
        s::Transform t;
        t.world[3] = glm::vec4(dx, 0, 0, 1);
        r.emplace<s::Transform>(e, t);
        s::Spline sp;
        sp.kind = s::SplineKind::polyline;
        sp.network = network;
        sp.id = id;
        sp.points = {{a, 1, 0, 0}, {b, 1, 0, 0}};
        r.emplace<s::Spline>(e, sp);
        return e;
    };
    const entt::entity a = make(1, 0, {-5, 0, 0}, {5, 0, 0}, 0.0F);
    make(1, 7, {0, 0, -5}, {0, 0, 5}, 2.0F);  // moved: crosses at world x = 2
    make(2, 0, {0, 0, -5}, {0, 0, 5}, 0.0F);  // another network: ignored
    auto net = s::build_curve_network(r, 1);
    ASSERT_TRUE(net.has_value()) << net.error().message;
    EXPECT_EQ(net->network.edges.size(), 4U);
    EXPECT_EQ(net->entities.size(), 2U);
    bool crossed = false;
    for (const auto& n : net->network.nodes) {
        if (n.kind == engine::spline::NodeKind::cross) {
            crossed = true;
            EXPECT_NEAR(n.position.x, 2.0F, 1e-4F);
        }
    }
    EXPECT_TRUE(crossed);

    r.get<s::Spline>(a).id = 7;
    EXPECT_NE(s::build_curve_network(r, 1).error().message.find("spline id 7 is used by entities"), std::string::npos);
    EXPECT_EQ(s::build_curve_network(r, 9).error().message, "network 9: no spline has it");
}

} // namespace
} // namespace quat::test
