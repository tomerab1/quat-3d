// Client prediction / reconciliation (S1 audit B5): the predictor keeps what
// it predicted per input, rewinds + replays only when the host disagrees, and
// ignores stale states. Transport- and physics-free (toy step).

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/net/prediction.hpp"

namespace {

using engine::net::CharacterPredictor;
using engine::net::CharacterState;
using engine::net::PlayerInput;

// Toy locomotion with gravity and a floor, so velocity and on_ground matter.
CharacterState toy_step(const CharacterState& s, const PlayerInput& in) {
    CharacterState n = s;
    n.velocity.x = in.move.x * 4.0F;
    n.velocity.z = in.move.z * 4.0F;
    n.velocity.y = s.on_ground ? 0.0F : s.velocity.y - 9.81F * in.dt;
    n.position += n.velocity * in.dt;
    n.on_ground = n.position.y <= 0.0F;
    if (n.on_ground) {
        n.position.y = 0.0F;
        n.velocity.y = 0.0F;
    }
    return n;
}

const CharacterPredictor::StepFn k_step = toy_step;
constexpr float k_dt = 1.0F / 60.0F;

struct Session {
    CharacterPredictor       predictor;
    CharacterState           client;
    std::vector<PlayerInput> inputs;

    void predict(std::uint32_t n, const glm::vec3& move) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const PlayerInput in{static_cast<std::uint32_t>(inputs.size()) + 1U, move, k_dt};
            inputs.push_back(in);
            client = predictor.predict(client, in, k_step);
        }
    }
    // The host's state after simulating inputs 1..acked from `start`.
    [[nodiscard]] CharacterState host_state(std::uint32_t acked,
                                            const CharacterState& start = {}) const {
        CharacterState s = start;
        for (std::uint32_t i = 0; i < acked; ++i) s = toy_step(s, inputs[i]);
        return s;
    }
};

TEST(NetPrediction, AgreeingHostCausesNoCorrection) {
    Session run;
    run.client.position.y = 2.0F; // falling, then walking
    const CharacterState start = run.client;
    run.predict(90, glm::vec3(1.0F, 0.0F, 0.0F));
    const CharacterState before = run.client;

    const auto corrected = run.predictor.reconcile(40U, run.host_state(40U, start), k_step);
    EXPECT_FALSE(corrected.has_value()) << "prediction matched, nothing to replay";
    EXPECT_EQ(run.predictor.pending(), 50U);
    EXPECT_EQ(run.predictor.corrections(), 0U);
    EXPECT_EQ(run.client.position, before.position);
}

TEST(NetPrediction, DivergenceRewindsAndReplays) {
    Session run;
    run.client.on_ground = true;
    run.predict(10, glm::vec3(0.0F, 0.0F, 1.0F));

    // The host was blocked: after input 4 it is half a metre behind, still
    // moving, and airborne (pushed) — every field of the state matters.
    CharacterState server = run.host_state(4U);
    server.position.z -= 0.5F;
    server.position.y = 0.3F;
    server.velocity.y = 1.0F;
    server.on_ground = false;

    const auto corrected = run.predictor.reconcile(4U, server, k_step);
    ASSERT_TRUE(corrected.has_value());
    CharacterState expected = server;
    for (std::uint32_t i = 4; i < 10; ++i) expected = toy_step(expected, run.inputs[i]);
    EXPECT_NEAR(glm::distance(corrected->position, expected.position), 0.0F, 1e-5F);
    EXPECT_NEAR(glm::distance(corrected->velocity, expected.velocity), 0.0F, 1e-5F);
    EXPECT_EQ(corrected->on_ground, expected.on_ground);
    EXPECT_EQ(run.predictor.pending(), 6U);

    // The replayed predictions are the new baseline: a host that now agrees
    // with the corrected timeline causes no second correction.
    CharacterState host = server;
    for (std::uint32_t i = 4; i < 7; ++i) host = toy_step(host, run.inputs[i]);
    EXPECT_FALSE(run.predictor.reconcile(7U, host, k_step).has_value());
}

TEST(NetPrediction, SmallDifferencesAreTolerated) {
    Session run;
    run.client.on_ground = true;
    run.predict(5, glm::vec3(1.0F, 0.0F, 0.0F));
    CharacterState server = run.host_state(3U, CharacterState{glm::vec3(0.0F), glm::vec3(0.0F), true});
    server.position.x += CharacterPredictor::position_tolerance * 0.5F;
    EXPECT_FALSE(run.predictor.reconcile(3U, server, k_step).has_value());

    server = run.host_state(4U, CharacterState{glm::vec3(0.0F), glm::vec3(0.0F), true});
    server.position.x += CharacterPredictor::position_tolerance * 2.0F;
    EXPECT_TRUE(run.predictor.reconcile(4U, server, k_step).has_value());
}

TEST(NetPrediction, StaleOrReorderedStatesAreIgnored) {
    Session run;
    run.client.on_ground = true;
    run.predict(10, glm::vec3(1.0F, 0.0F, 0.0F));
    const CharacterState start{glm::vec3(0.0F), glm::vec3(0.0F), true};
    EXPECT_FALSE(run.predictor.reconcile(6U, run.host_state(6U, start), k_step).has_value());
    // An older state arriving late (unreliable channel) must not rewind.
    CharacterState old = run.host_state(3U, start);
    old.position.x -= 5.0F;
    EXPECT_FALSE(run.predictor.reconcile(3U, old, k_step).has_value());
    EXPECT_FALSE(run.predictor.reconcile(6U, old, k_step).has_value());
    EXPECT_EQ(run.predictor.pending(), 4U);
}

TEST(NetPrediction, AckForADroppedInputCorrects) {
    Session run;
    run.client.on_ground = true;
    run.predict(static_cast<std::uint32_t>(CharacterPredictor::max_pending) + 20U,
                glm::vec3(1.0F, 0.0F, 0.0F));
    ASSERT_EQ(run.predictor.dropped(), 20U);
    // The host acks input 5, which the client no longer remembers: it cannot
    // verify the prediction, so it trusts the host and replays the rest.
    const auto corrected = run.predictor.reconcile(5U, run.host_state(5U), k_step);
    EXPECT_TRUE(corrected.has_value());
    EXPECT_EQ(run.predictor.pending(), CharacterPredictor::max_pending);
}

TEST(NetPrediction, StateWireRoundTrip) {
    const CharacterState s{glm::vec3(1.0F, 2.0F, 3.0F), glm::vec3(-4.0F, 5.0F, 0.5F), true};
    std::vector<std::uint8_t> wire = engine::net::encode_character_state(99U, s);
    std::uint32_t acked = 0;
    CharacterState out;
    ASSERT_TRUE(engine::net::decode_character_state(wire, acked, out));
    EXPECT_EQ(acked, 99U);
    EXPECT_EQ(out.position, s.position);
    EXPECT_EQ(out.velocity, s.velocity);
    EXPECT_TRUE(out.on_ground);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(wire.data() + 17, &nan, 4);
    EXPECT_FALSE(engine::net::decode_character_state(wire, acked, out));
    wire = engine::net::encode_character_state(99U, s);
    wire.pop_back();
    EXPECT_FALSE(engine::net::decode_character_state(wire, acked, out));
}

} // namespace
