// Behaviour trees driving NavAgents on a baked navmesh (move_to end to end).

#include <optional>
#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/core/json_fields.hpp"
#include "engine/scene/behavior.hpp"
#include "nav/nav_fixtures.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using quat::test::position_of;
using quat::test::xz_distance;

constexpr float kDt = 1.0F / 60.0F;

// 40 x 40 m floor (top at y = 0) with a 3 m wall across z = 0 from x = -20 to
// x = 12, leaving a gap on the +x side.
struct WalledPlate {
    entt::registry registry;
    engine::nav::NavMesh navmesh;
    entt::entity agent = entt::null;
    bool flag = false; // read by the "flag" test leaf
    s::BtLeafFactories leaves;

    WalledPlate() {
        quat::test::add_box(registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
        quat::test::add_box(registry, glm::vec3(-4.0F, 1.5F, 0.0F), glm::vec3(16.0F, 1.5F, 0.5F));
        auto baked = quat::test::bake(registry);
        EXPECT_OK(baked);
        if (baked) navmesh = std::move(*baked);
        agent = quat::test::place(registry, glm::vec3(-10.0F, 0.0F, -10.0F));
        registry.emplace<s::NavAgent>(agent);
        leaves["flag"] = [this](engine::core::JsonFields&)
            -> std::expected<s::BtLeaf, engine::core::Error> {
            s::BtLeaf leaf;
            leaf.tick = [this](s::BtLeafContext&) {
                return flag ? s::BtStatus::success : s::BtStatus::failure;
            };
            return leaf;
        };
    }

    s::BehaviorTree& tree(const char* json) {
        auto& bt = registry.emplace_or_replace<s::BehaviorTree>(agent);
        bt.source = json;
        auto parsed = s::BehaviorTreeAsset::parse(json, &leaves);
        EXPECT_OK(parsed);
        if (parsed) bt.compiled = *parsed;
        return bt;
    }

    // One frame: tree first, then navigation (the frame loop's order).
    s::BtStatus step() {
        const s::BtStatus st =
            s::bt_tick(registry.get<s::BehaviorTree>(agent), registry, agent, s::BtWorld{}, kDt);
        s::nav_agent_system(registry, navmesh, kDt);
        return st;
    }

    [[nodiscard]] glm::vec3 pos() const { return position_of(registry, agent); }
    s::NavAgent& nav() { return registry.get<s::NavAgent>(agent); }
};

// Regression (S1 N1): sequence[move_to A, wait, move_to B] used to re-aim at
// A every frame and never get past it.
TEST(Ai, SequencePatrolVisitsBothPointsInOrder) {
    WalledPlate w;
    ASSERT_TRUE(w.navmesh.valid());
    w.tree(R"({"type":"sequence","children":[
        {"type":"leaf","name":"move_to","x":-10,"y":0,"z":10,"speed":6},
        {"type":"leaf","name":"wait","seconds":0.5},
        {"type":"leaf","name":"move_to","x":15,"y":0,"z":10,"speed":6}]})");

    bool reached_a = false;
    std::optional<s::BtStatus> result;
    for (int i = 0; i < 60 * 60 && !result; ++i) {
        const s::BtStatus st = w.step();
        reached_a = reached_a || xz_distance(w.pos(), {-10.0F, 0.0F, 10.0F}) < 1.0F;
        if (st != s::BtStatus::running) result = st;
    }
    ASSERT_TRUE(result.has_value()) << "the patrol never finished";
    EXPECT_EQ(*result, s::BtStatus::success);
    EXPECT_TRUE(reached_a);
    EXPECT_LT(xz_distance(w.pos(), {15.0F, 0.0F, 10.0F}), 1.0F);
}

// The old self-test's tree (selector[near point -> wait, else move_to]).
TEST(Ai, SelectorWalksThenWaitsAtThePoint) {
    WalledPlate w;
    auto& bt = w.tree(R"({"type":"selector","children":[
        {"type":"sequence","children":[
            {"type":"leaf","name":"near_point","x":-10,"z":10,"range":1.5},
            {"type":"leaf","name":"wait","seconds":0.5}]},
        {"type":"leaf","name":"move_to","x":-10,"y":0,"z":10,"speed":4}]})");
    for (int i = 0; i < 60 * 30; ++i) w.step();
    EXPECT_LT(xz_distance(w.pos(), {-10.0F, 0.0F, 10.0F}), 1.5F);
    // Node order (DFS): 0 selector, 1 sequence, 2 near_point, 3 wait, 4 move_to.
    bool waiting = false;
    for (int i = 0; i < 20 && !waiting; ++i) {
        w.step();
        waiting = bt.node_state[3].running;
    }
    EXPECT_TRUE(waiting) << "the wait branch runs once the agent is at the point";
}

// Regression (S1 N1): a pre-empted move_to kept its agent walking.
TEST(Ai, PreemptedMoveToStopsTheAgent) {
    WalledPlate w;
    w.tree(R"({"type":"reactive_selector","children":[
        {"type":"leaf","name":"flag"},
        {"type":"leaf","name":"move_to","x":15,"y":0,"z":15,"speed":4}]})");
    for (int i = 0; i < 30; ++i) EXPECT_EQ(w.step(), s::BtStatus::running);
    ASSERT_TRUE(w.nav().active);

    w.flag = true;
    EXPECT_EQ(w.step(), s::BtStatus::success);
    EXPECT_FALSE(w.nav().active) << "pre-emption halts the walk";
    const glm::vec3 stopped = w.pos();
    for (int i = 0; i < 30; ++i) w.step();
    EXPECT_LT(xz_distance(w.pos(), stopped), 1e-3F);

    w.flag = false; // resumes: move_to aims afresh
    w.step();
    EXPECT_TRUE(w.nav().active);
}

// move_to stays succeeded while a reactive parent keeps re-ticking it.
TEST(Ai, MoveToDoesNotReaimWhileHeld) {
    WalledPlate w;
    w.registry.get<s::Transform>(w.agent).world[3] = glm::vec4(-10.0F, 0.0F, -12.0F, 1.0F);
    w.tree(R"({"type":"reactive_sequence","children":[
        {"type":"leaf","name":"move_to","x":-10,"y":0,"z":-10,"speed":4},
        {"type":"leaf","name":"wait","seconds":2}]})");
    int arrived_frames = 0;
    for (int i = 0; i < 60 * 2; ++i) {
        w.step();
        if (w.nav().arrived) ++arrived_frames;
        if (arrived_frames > 0) {
            EXPECT_TRUE(w.nav().arrived) << "frame " << i << ": re-aimed after success";
        }
    }
    EXPECT_GT(arrived_frames, 60);
}

// Regression (S1 N4): move_to never reported failure, so a selector could
// not fall back from an unreachable destination.
TEST(Ai, UnreachableMoveToFallsBackInASelector) {
    WalledPlate w;
    w.tree(R"({"type":"selector","children":[
        {"type":"leaf","name":"move_to","x":0,"y":40,"z":-10,"speed":6},
        {"type":"leaf","name":"move_to","x":5,"y":0,"z":-10,"speed":6}]})");
    std::optional<s::BtStatus> result;
    for (int i = 0; i < 60 * 30 && !result; ++i) {
        const s::BtStatus st = w.step();
        if (st != s::BtStatus::running) result = st;
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, s::BtStatus::success) << "the fallback branch must run";
    EXPECT_LT(xz_distance(w.pos(), {5.0F, 0.0F, -10.0F}), 1.0F);
}

// Regression (S1 N4): a move_to without y aimed at y = 0, which is off the
// mesh on raised ground (beyond the endpoint snapping box).
TEST(Ai, MoveToWithoutYSnapsToRaisedGround) {
    entt::registry r;
    constexpr float top = 30.0F;
    quat::test::add_box(r, glm::vec3(0.0F, top - 0.5F, 0.0F), glm::vec3(15.0F, 0.5F, 15.0F));
    auto mesh = quat::test::bake(r);
    ASSERT_OK(mesh);
    const entt::entity e = quat::test::place(r, glm::vec3(-8.0F, top, -8.0F));
    r.emplace<s::NavAgent>(e);
    auto& bt = r.emplace<s::BehaviorTree>(e);
    auto parsed = s::BehaviorTreeAsset::parse(
        R"({"type":"leaf","name":"move_to","x":8,"z":8,"speed":6})");
    ASSERT_OK(parsed);
    bt.compiled = *parsed;

    const s::BtWorld world{.navmesh = &*mesh};
    std::optional<s::BtStatus> result;
    for (int i = 0; i < 60 * 20 && !result; ++i) {
        const s::BtStatus st = s::bt_tick(bt, r, e, world, kDt);
        s::nav_agent_system(r, *mesh, kDt);
        if (st != s::BtStatus::running) result = st;
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, s::BtStatus::success);
    EXPECT_NEAR(r.get<s::NavAgent>(e).target.y, top, 0.5F);
    EXPECT_LT(xz_distance(position_of(r, e), {8.0F, top, 8.0F}), 1.0F);
}

} // namespace
