// Behaviour-tree flow control with scripted ("probe") leaves — no navmesh.
//
// A probe {id, run, result} returns running for `run` ticks, then `result`
// (success/failure), and logs every tick and halt by id. A "flag" leaf
// succeeds while its named test flag is set.

#include <map>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/core/json_fields.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;

struct ProbeLog {
    int ticks = 0;
    int halts = 0;
};

struct Harness {
    std::map<std::string, ProbeLog> log;
    std::map<std::string, bool>     flags;
    std::vector<std::string>        trace; // ids in tick order
    s::BtLeafFactories              leaves;

    Harness() {
        leaves["probe"] = [this](engine::core::JsonFields& f)
            -> std::expected<s::BtLeaf, engine::core::Error> {
            std::string id;
            int run = 0;
            std::string result = "success";
            f.string("id", id).integer("run", run, 0, 1000).string("result", result);
            const s::BtStatus done =
                result == "failure" ? s::BtStatus::failure : s::BtStatus::success;
            s::BtLeaf leaf;
            leaf.tick = [this, id, run, done](s::BtLeafContext& c) {
                ++log[id].ticks;
                trace.push_back(id);
                c.state += 1.0F;
                return c.state > static_cast<float>(run) ? done : s::BtStatus::running;
            };
            leaf.halt = [this, id](s::BtLeafContext&) { ++log[id].halts; };
            return leaf;
        };
        leaves["flag"] = [this](engine::core::JsonFields& f)
            -> std::expected<s::BtLeaf, engine::core::Error> {
            std::string id;
            f.string("id", id);
            s::BtLeaf leaf;
            leaf.tick = [this, id](s::BtLeafContext&) {
                trace.push_back(id);
                return flags[id] ? s::BtStatus::success : s::BtStatus::failure;
            };
            return leaf;
        };
    }

    entt::registry registry;
    entt::entity   entity = registry.create();
    s::BehaviorTree bt;

    [[nodiscard]] bool load(const char* json) {
        auto parsed = s::BehaviorTreeAsset::parse(json, &leaves);
        EXPECT_OK(parsed);
        if (!parsed) return false;
        bt.compiled = *parsed;
        return true;
    }
    s::BtStatus tick() {
        trace.clear();
        return s::bt_tick(bt, registry, entity, s::BtWorld{}, 1.0F / 60.0F);
    }
};

TEST(Ai, ParseRejectsMalformedTrees) {
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += R"({"type":"inverter","children":[)";
    deep += R"({"type":"leaf","name":"wait"})";
    for (int i = 0; i < 200; ++i) deep += "]}";
    const char* const bad[] = {
        R"({"type":1})",
        R"({"type":"leaf","name":"wait","seconds":"2"})",
        R"({"type":"leaf","name":"wait","second":2})",
        R"({"type":"leaf","name":"wait","seconds":-1})",
        R"({"type":"leaf","name":7})",
        R"({"type":"leaf","name":"move_to","x":[1]})",
        R"({"type":"sequence","children":{}})",
        R"({"type":"sequence","children":[5]})",
        R"({"type":"reactive_selector","children":[]})",
        R"({"type":"parallel","children":[{"type":"leaf","name":"wait"}]})",
        R"([1,2,3])",
        deep.c_str(),
    };
    for (const char* text : bad) {
        EXPECT_ERR(s::BehaviorTreeAsset::parse(text)) << text;
    }
    EXPECT_OK(s::BehaviorTreeAsset::parse(R"({"type":"leaf","name":"wait","seconds":2})"));
    EXPECT_OK(s::BehaviorTreeAsset::parse(
        R"({"type":"reactive_sequence","children":[{"type":"leaf","name":"near_camera"}]})"));
}

// Regression (S1 N1): a sequence re-ran finished steps every tick, so
// sequence[move_to A, wait, move_to B] re-aimed at A forever.
TEST(Ai, SequenceResumesAtTheRunningChild) {
    Harness h;
    ASSERT_TRUE(h.load(R"({"type":"sequence","children":[
        {"type":"leaf","name":"probe","id":"a","run":2},
        {"type":"leaf","name":"probe","id":"b","run":3},
        {"type":"leaf","name":"probe","id":"c","run":0}]})"));

    EXPECT_EQ(h.tick(), s::BtStatus::running); // a 1
    EXPECT_EQ(h.tick(), s::BtStatus::running); // a 2
    EXPECT_EQ(h.tick(), s::BtStatus::running); // a done, b 1
    EXPECT_EQ(h.trace, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(h.tick(), s::BtStatus::running); // b 2 — a is not re-run
    EXPECT_EQ(h.trace, (std::vector<std::string>{"b"}));
    EXPECT_EQ(h.tick(), s::BtStatus::running); // b 3
    EXPECT_EQ(h.tick(), s::BtStatus::success); // b done, c done
    EXPECT_EQ(h.log["a"].ticks, 3);
    EXPECT_EQ(h.log["b"].ticks, 4);
    EXPECT_EQ(h.log["c"].ticks, 1);
    EXPECT_EQ(h.log["a"].halts + h.log["b"].halts + h.log["c"].halts, 0)
        << "finished leaves are never halted";

    // Finished: the next tick starts over from the first step.
    EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.trace, (std::vector<std::string>{"a"}));
}

TEST(Ai, SelectorResumesAtTheRunningChildAndFallsBack) {
    Harness h;
    ASSERT_TRUE(h.load(R"({"type":"selector","children":[
        {"type":"leaf","name":"probe","id":"first","run":2,"result":"failure"},
        {"type":"leaf","name":"probe","id":"fallback","run":1}]})"));
    EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.tick(), s::BtStatus::running); // first fails -> fallback starts
    EXPECT_EQ(h.trace, (std::vector<std::string>{"first", "fallback"}));
    EXPECT_EQ(h.tick(), s::BtStatus::success);
    EXPECT_EQ(h.trace, (std::vector<std::string>{"fallback"}));
    EXPECT_EQ(h.log["first"].ticks, 3);
}

// Regression (S1 N1): pre-empted leaves kept their memory and were never
// told to stop (a pre-empted move_to kept walking).
TEST(Ai, ReactiveSelectorPreemptsAndResetsTheRunningBranch) {
    Harness h;
    ASSERT_TRUE(h.load(R"({"type":"reactive_selector","children":[
        {"type":"leaf","name":"flag","id":"alert"},
        {"type":"sequence","children":[
            {"type":"leaf","name":"probe","id":"walk_a","run":1},
            {"type":"leaf","name":"probe","id":"walk_b","run":5}]}]})"));

    for (int i = 0; i < 4; ++i) EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.log["walk_b"].ticks, 3);

    h.flags["alert"] = true; // the higher-priority branch takes over
    EXPECT_EQ(h.tick(), s::BtStatus::success);
    EXPECT_EQ(h.log["walk_b"].halts, 1) << "the running leaf is halted";
    EXPECT_EQ(h.log["walk_a"].halts, 0) << "finished leaves are not";
    EXPECT_EQ(h.tick(), s::BtStatus::success);
    EXPECT_EQ(h.log["walk_b"].halts, 1) << "halted once, not every tick";

    h.flags["alert"] = false; // the patrol restarts from scratch
    EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.trace, (std::vector<std::string>{"alert", "walk_a"}));
    EXPECT_EQ(h.tick(), s::BtStatus::running);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.tick(), s::BtStatus::success) << "walk_b ran its full 6 ticks again";
}

TEST(Ai, ReactiveSequenceAbortsWhenItsConditionFails) {
    Harness h;
    h.flags["see_player"] = true;
    ASSERT_TRUE(h.load(R"({"type":"reactive_sequence","children":[
        {"type":"leaf","name":"flag","id":"see_player"},
        {"type":"leaf","name":"probe","id":"chase","run":100}]})"));
    for (int i = 0; i < 3; ++i) EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.trace, (std::vector<std::string>{"see_player", "chase"}));

    h.flags["see_player"] = false;
    EXPECT_EQ(h.tick(), s::BtStatus::failure);
    EXPECT_EQ(h.log["chase"].halts, 1);
}

TEST(Ai, InverterAndCompositeCompletionReset) {
    Harness h;
    // inverter(failure) = success; the sequence then completes and restarts.
    ASSERT_TRUE(h.load(R"({"type":"sequence","children":[
        {"type":"inverter","children":[
            {"type":"leaf","name":"probe","id":"x","run":1,"result":"failure"}]},
        {"type":"leaf","name":"probe","id":"y","run":0}]})"));
    EXPECT_EQ(h.tick(), s::BtStatus::running);
    EXPECT_EQ(h.tick(), s::BtStatus::success);
    EXPECT_EQ(h.tick(), s::BtStatus::running) << "x starts over (its memory was reset)";
    EXPECT_EQ(h.trace, (std::vector<std::string>{"x"}));
}

} // namespace
