// Host-side simulation of remote input (S1 audit B1): every received input is
// simulated (none collapse), no faster than real time, and only simulated
// inputs are acknowledged. Client side: the pending queue is bounded.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/net/prediction.hpp"

namespace {

using engine::net::CharacterPredictor;
using engine::net::InputQueue;
using engine::net::PlayerInput;

constexpr float k_frame = 1.0F / 60.0F;

// Host loop as NetSession runs it: credit the frame, then simulate every
// input that is due (here: a toy step that records what ran).
std::vector<PlayerInput> host_frame(InputQueue& q, float dt) {
    std::vector<PlayerInput> stepped;
    q.credit(dt);
    while (const auto in = q.take()) stepped.push_back(*in);
    return stepped;
}

TEST(NetHostInput, SeveralInputsPerPollAreEachSimulated) {
    InputQueue q;
    // Three inputs arrive in one poll (network bunching), all different.
    ASSERT_EQ(q.push({1U, glm::vec3(1.0F, 0.0F, 0.0F), k_frame}), InputQueue::Push::queued);
    ASSERT_EQ(q.push({2U, glm::vec3(0.0F, 0.0F, 1.0F), k_frame}), InputQueue::Push::queued);
    ASSERT_EQ(q.push({3U, glm::vec3(-1.0F, 0.0F, 0.0F), k_frame}), InputQueue::Push::queued);
    EXPECT_EQ(q.acked(), 0U) << "nothing simulated, nothing acknowledged";

    // The host had been idle, so its budget covers the burst.
    const std::vector<PlayerInput> stepped = host_frame(q, 3.0F * k_frame);
    ASSERT_EQ(stepped.size(), 3U);
    EXPECT_EQ(stepped[0].move, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(stepped[1].move, glm::vec3(0.0F, 0.0F, 1.0F));
    EXPECT_EQ(stepped[2].move, glm::vec3(-1.0F, 0.0F, 0.0F));
    EXPECT_EQ(q.acked(), 3U);
}

TEST(NetHostInput, AcksOnlyWhatWasSimulated) {
    InputQueue q;
    for (std::uint32_t s = 1; s <= 5; ++s) {
        ASSERT_EQ(q.push({s, glm::vec3(1.0F, 0.0F, 0.0F), k_frame}), InputQueue::Push::queued);
    }
    // One frame of host time simulates one 60 Hz input; the ack says so even
    // though five were received.
    EXPECT_EQ(host_frame(q, k_frame).size(), 1U);
    EXPECT_EQ(q.acked(), 1U);
    EXPECT_EQ(q.last_received(), 5U);
    EXPECT_EQ(q.size(), 4U);
}

TEST(NetHostInput, SimulatesNoFasterThanRealTime) {
    InputQueue q;
    std::uint32_t seq = 0;
    float simulated = 0.0F;
    // A cheating client sends ten inputs per host frame.
    for (int frame = 0; frame < 120; ++frame) {
        for (int i = 0; i < 10; ++i) {
            (void)q.push({++seq, glm::vec3(1.0F, 0.0F, 0.0F), k_frame});
        }
        for (const PlayerInput& in : host_frame(q, k_frame)) simulated += in.dt;
    }
    // Two seconds of host time simulate at most two seconds (+ the jitter
    // budget) of client movement, and the queue stays bounded.
    EXPECT_LE(simulated, 2.0F + InputQueue::max_time_budget + 1e-3F);
    EXPECT_GE(simulated, 2.0F - 2.0F * k_frame);
    EXPECT_LE(q.size(), InputQueue::capacity);
}

TEST(NetHostInput, LateInputsCatchUpWithinTheJitterBudget) {
    InputQueue q;
    // 0.2 s with no input (network stall): budget accrues, capped.
    for (int frame = 0; frame < 12; ++frame) EXPECT_TRUE(host_frame(q, k_frame).empty());
    std::uint32_t seq = 0;
    for (int i = 0; i < 13; ++i) {
        (void)q.push({++seq, glm::vec3(0.0F, 0.0F, 1.0F), k_frame});
    }
    // The delayed inputs are simulated at once (0.2 s banked + this frame).
    EXPECT_EQ(host_frame(q, k_frame).size(), 13U);

    // A long stall cannot bank more than max_time_budget.
    for (int frame = 0; frame < 600; ++frame) (void)host_frame(q, k_frame);
    for (int i = 0; i < 60; ++i) {
        (void)q.push({++seq, glm::vec3(0.0F, 0.0F, 1.0F), k_frame});
    }
    const std::size_t burst = host_frame(q, k_frame).size();
    EXPECT_LE(static_cast<float>(burst) * k_frame,
              InputQueue::max_time_budget + k_frame + 1e-3F);
}

TEST(NetHostInput, ResetForgetsTheOldOwner) {
    InputQueue q;
    (void)q.push({7U, glm::vec3(0.0F), k_frame});
    (void)host_frame(q, k_frame);
    EXPECT_EQ(q.acked(), 7U);
    q.reset();
    EXPECT_EQ(q.acked(), 0U);
    EXPECT_TRUE(host_frame(q, 0.0F).empty());
}

TEST(NetClientInput, PendingQueueIsBounded) {
    const CharacterPredictor::StepFn step = [](const engine::net::CharacterState& s,
                                               const PlayerInput& in) {
        engine::net::CharacterState next = s;
        next.position += in.move * in.dt;
        return next;
    };
    CharacterPredictor predictor;
    engine::net::CharacterState state;
    // The host stops acknowledging (stall / packet loss): memory stays bounded.
    for (std::uint32_t s = 1; s <= 1000; ++s) {
        state = predictor.predict(state, {s, glm::vec3(1.0F, 0.0F, 0.0F), k_frame}, step);
    }
    EXPECT_EQ(predictor.pending(), CharacterPredictor::max_pending);
    EXPECT_EQ(predictor.dropped(), 1000U - CharacterPredictor::max_pending);
    (void)predictor.reconcile(1000U, state, step);
    EXPECT_EQ(predictor.pending(), 0U);
}

} // namespace
