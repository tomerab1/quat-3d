// Untrusted player input (S1 audit B2): everything from the wire is
// validated before the host simulates it.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/net/prediction.hpp"

namespace {

using engine::net::InputQueue;
using engine::net::PlayerInput;
using engine::net::sanitize_input;

constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float k_inf = std::numeric_limits<float>::infinity();

TEST(NetInput, RejectsNonFiniteValues) {
    for (const glm::vec3 move : {glm::vec3(k_nan, 0.0F, 0.0F), glm::vec3(0.0F, k_inf, 0.0F),
                                 glm::vec3(0.0F, 0.0F, -k_inf)}) {
        PlayerInput in{1U, move, 1.0F / 60.0F};
        EXPECT_FALSE(sanitize_input(in));
    }
    for (const float dt : {k_nan, k_inf, 0.0F, -0.01F}) {
        PlayerInput in{1U, glm::vec3(1.0F, 0.0F, 0.0F), dt};
        EXPECT_FALSE(sanitize_input(in)) << "dt " << dt;
    }
}

TEST(NetInput, ClampsSpeedHacks) {
    // A huge (finite) move must not become a speed multiplier, nor overflow
    // the length computation to infinity.
    PlayerInput in{1U, glm::vec3(3e38F, 5.0F, -3e38F), 1.0F / 60.0F};
    ASSERT_TRUE(sanitize_input(in));
    EXPECT_EQ(in.move.y, 0.0F);
    EXPECT_NEAR(glm::length(in.move), 1.0F, 1e-5F);
    EXPECT_NEAR(in.move.x, std::sqrt(0.5F), 1e-5F);
    EXPECT_NEAR(in.move.z, -std::sqrt(0.5F), 1e-5F);

    PlayerInput slow{2U, glm::vec3(0.3F, 0.0F, 0.4F), 1.0F / 60.0F};
    ASSERT_TRUE(sanitize_input(slow));
    EXPECT_NEAR(slow.move.x, 0.3F, 1e-6F) << "analog input below 1 is kept";
    EXPECT_NEAR(slow.move.z, 0.4F, 1e-6F);

    PlayerInput long_frame{3U, glm::vec3(1.0F, 0.0F, 0.0F), 10.0F};
    ASSERT_TRUE(sanitize_input(long_frame));
    EXPECT_EQ(long_frame.dt, engine::net::max_input_dt);
}

TEST(NetInput, DecodeSanitizes) {
    PlayerInput hostile{9U, glm::vec3(100.0F, 100.0F, 0.0F), 5.0F};
    std::vector<std::uint8_t> wire = engine::net::encode_input(hostile);
    PlayerInput out;
    ASSERT_TRUE(engine::net::decode_input(wire, out));
    EXPECT_EQ(out.sequence, 9U);
    EXPECT_EQ(out.move, glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(out.dt, engine::net::max_input_dt);

    // NaN on the wire is refused and leaves `out` untouched.
    const float nan = k_nan;
    std::memcpy(wire.data() + 5, &nan, 4);
    PlayerInput untouched{77U, glm::vec3(0.0F), 0.5F};
    EXPECT_FALSE(engine::net::decode_input(wire, untouched));
    EXPECT_EQ(untouched.sequence, 77U);

    // Wrong size / kind.
    EXPECT_FALSE(engine::net::decode_input({2, 0, 0}, out));
    wire = engine::net::encode_input(hostile);
    wire[0] = 3;
    EXPECT_FALSE(engine::net::decode_input(wire, out));
}

TEST(NetInput, MakeInputUsesTheHostRules) {
    const auto in = engine::net::make_input(4U, glm::vec3(0.0F, 0.0F, 2.0F), 0.5F);
    ASSERT_TRUE(in.has_value());
    EXPECT_EQ(in->move, glm::vec3(0.0F, 0.0F, 1.0F));
    EXPECT_EQ(in->dt, engine::net::max_input_dt);
    EXPECT_FALSE(engine::net::make_input(5U, glm::vec3(0.0F), 0.0F).has_value());
}

TEST(NetInput, QueueRequiresIncreasingSequences) {
    InputQueue q;
    const glm::vec3 fwd(0.0F, 0.0F, 1.0F);
    EXPECT_EQ(q.push({0U, fwd, 0.016F}), InputQueue::Push::stale) << "sequences start at 1";
    EXPECT_EQ(q.push({1U, fwd, 0.016F}), InputQueue::Push::queued);
    EXPECT_EQ(q.push({3U, fwd, 0.016F}), InputQueue::Push::queued) << "gaps are fine";
    EXPECT_EQ(q.push({3U, fwd, 0.016F}), InputQueue::Push::stale) << "replay";
    EXPECT_EQ(q.push({2U, fwd, 0.016F}), InputQueue::Push::stale) << "reordered";
    EXPECT_EQ(q.push({4U, glm::vec3(k_nan), 0.016F}), InputQueue::Push::malformed);
    EXPECT_EQ(q.size(), 2U);
    EXPECT_EQ(q.last_received(), 3U);
    q.reset();
    EXPECT_EQ(q.size(), 0U);
    EXPECT_EQ(q.push({1U, fwd, 0.016F}), InputQueue::Push::queued) << "fresh owner";
}

TEST(NetInput, QueueIsBounded) {
    InputQueue q;
    std::uint32_t seq = 0;
    for (std::size_t i = 0; i < InputQueue::capacity; ++i) {
        ASSERT_EQ(q.push({++seq, glm::vec3(0.0F), 0.016F}), InputQueue::Push::queued);
    }
    EXPECT_EQ(q.push({++seq, glm::vec3(0.0F), 0.016F}), InputQueue::Push::full);
    EXPECT_EQ(q.size(), InputQueue::capacity);
}

} // namespace
