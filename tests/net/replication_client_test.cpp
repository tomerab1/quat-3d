// Client side of replication (S1 audit B4): replicated bodies are kinematic,
// world poses are converted through the parent, the smoothed pose is owned by
// the receiver (local writes don't fight it), and snapshots are validated.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include "engine/net/replication.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"

namespace {

namespace s = engine::scene;
using engine::net::ReplicationClient;
using engine::net::ReplicationServer;

constexpr float k_dt = 1.0F / 60.0F;

// A snapshot packet carrying one world pose (built by the real server).
std::vector<std::uint8_t> snapshot(std::uint32_t id, const glm::vec3& pos, const glm::quat& rot) {
    entt::registry server;
    const entt::entity e = server.create();
    server.emplace<s::NetReplicated>(e, id);
    auto& t = server.emplace<s::Transform>(e);
    t.world = glm::translate(glm::mat4(1.0F), pos) * glm::mat4_cast(rot);
    t.local = t.world;
    ReplicationServer rs(20.0F);
    auto packets = rs.collect(server, 0.05F);
    EXPECT_EQ(packets.size(), 1U);
    return packets.empty() ? std::vector<std::uint8_t>{} : packets[0].bytes;
}

void run_frames(ReplicationClient& rc, entt::registry& r, int frames) {
    for (int i = 0; i < frames; ++i) rc.apply(r, k_dt);
}

void expect_near(const glm::vec3& a, const glm::vec3& b, float tol = 1e-3F) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

TEST(NetReplicaClient, ChildWorldPoseIsConvertedThroughItsParent) {
    entt::registry r;
    const entt::entity parent = r.create();
    auto& pt = r.emplace<s::Transform>(parent);
    pt.local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, 0.0F)) *
               glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0.0F, 1.0F, 0.0F)) *
               glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    pt.world = pt.local;
    const entt::entity child = r.create();
    auto& ct = r.emplace<s::Transform>(child);
    ct.local = glm::scale(glm::mat4(1.0F), glm::vec3(0.5F)); // world scale 1
    ct.world = pt.world * ct.local;
    r.emplace<s::Parent>(child, parent);
    r.emplace<s::NetReplicated>(child, 5U);

    const glm::vec3 target(3.0F, 4.0F, 5.0F);
    const glm::quat rot = glm::angleAxis(glm::radians(30.0F), glm::vec3(1.0F, 0.0F, 0.0F));
    ReplicationClient rc;
    rc.receive(snapshot(5U, target, rot));
    run_frames(rc, r, 120);

    // The world pose is the server's; the local reproduces it through the
    // parent (what transform_system will compute next tick).
    expect_near(glm::vec3(ct.world[3]), target);
    expect_near(glm::vec3((pt.world * ct.local)[3]), target);
    const glm::mat4 recomposed = pt.world * ct.local;
    for (int c = 0; c < 3; ++c) {
        expect_near(glm::vec3(recomposed[c]), glm::vec3(ct.world[c]), 1e-3F);
    }
    EXPECT_NEAR(glm::length(glm::vec3(ct.world[0])), 1.0F, 1e-4F) << "world scale kept";
}

TEST(NetReplicaClient, ReplicatedParentsResolveBeforeChildren) {
    entt::registry r;
    // Create the child first so registry order is child-before-parent.
    const entt::entity child = r.create();
    const entt::entity parent = r.create();
    r.emplace<s::Transform>(child);
    r.emplace<s::Transform>(parent);
    r.emplace<s::Parent>(child, parent);
    r.emplace<s::NetReplicated>(child, 1U);
    r.emplace<s::NetReplicated>(parent, 2U);

    ReplicationClient rc;
    rc.receive(snapshot(2U, glm::vec3(100.0F, 0.0F, 0.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F)));
    rc.receive(snapshot(1U, glm::vec3(101.0F, 2.0F, 0.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F)));
    // A single frame: the child's local must be computed against the parent's
    // pose of THIS frame, not last frame's.
    rc.apply(r, k_dt);
    const glm::mat4 parent_world = r.get<s::Transform>(parent).local;
    const glm::mat4 child_world = parent_world * r.get<s::Transform>(child).local;
    expect_near(glm::vec3(child_world[3]), glm::vec3(r.get<s::Transform>(child).world[3]), 1e-3F);
}

TEST(NetReplicaClient, LocalWritesDoNotFightTheStream) {
    entt::registry r;
    const entt::entity e = r.create();
    auto& t = r.emplace<s::Transform>(e);
    r.emplace<s::NetReplicated>(e, 9U);
    ReplicationClient rc;
    const glm::vec3 target(0.0F, 5.0F, 0.0F);
    rc.receive(snapshot(9U, target, glm::quat(1.0F, 0.0F, 0.0F, 0.0F)));
    for (int i = 0; i < 120; ++i) {
        // A local system (e.g. the demo spinner) keeps writing its own pose.
        t.local = glm::translate(glm::mat4(1.0F), glm::vec3(-50.0F, 0.0F, 0.0F));
        t.world = t.local;
        rc.apply(r, k_dt);
    }
    expect_near(glm::vec3(t.world[3]), target);
    expect_near(glm::vec3(t.local[3]), target);
}

TEST(NetReplicaClient, SmoothingIsFrameRateIndependent) {
    const auto converge = [](float dt, int frames) {
        entt::registry r;
        const entt::entity e = r.create();
        r.emplace<s::Transform>(e);
        r.emplace<s::NetReplicated>(e, 3U);
        ReplicationClient rc;
        rc.receive(snapshot(3U, glm::vec3(10.0F, 0.0F, 0.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F)));
        for (int i = 0; i < frames; ++i) rc.apply(r, dt);
        return r.get<s::Transform>(e).world[3].x;
    };
    // 0.1 s at 30, 60 and 240 fps lands at the same point: ~95 % of the way.
    const float at30 = converge(1.0F / 30.0F, 3);
    EXPECT_NEAR(converge(1.0F / 60.0F, 6), at30, 1e-3F);
    EXPECT_NEAR(converge(1.0F / 240.0F, 24), at30, 1e-3F);
    EXPECT_NEAR(at30, 9.5F, 0.05F);
}

TEST(NetReplicaClient, MalformedSnapshotsAreIgnored) {
    entt::registry r;
    const entt::entity e = r.create();
    auto& t = r.emplace<s::Transform>(e);
    r.emplace<s::NetReplicated>(e, 4U);
    ReplicationClient rc;

    std::vector<std::uint8_t> bad = snapshot(4U, glm::vec3(1.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(bad.data() + engine::net::snapshot_header_bytes + 4, &nan, 4);
    rc.receive(bad);
    std::vector<std::uint8_t> truncated =
        snapshot(4U, glm::vec3(1.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F));
    truncated.pop_back();
    rc.receive(truncated);
    std::vector<std::uint8_t> zero_quat =
        snapshot(4U, glm::vec3(1.0F), glm::quat(1.0F, 0.0F, 0.0F, 0.0F));
    std::memset(zero_quat.data() + engine::net::snapshot_header_bytes + 16, 0, 16);
    rc.receive(zero_quat);

    run_frames(rc, r, 10);
    EXPECT_EQ(glm::vec3(t.world[3]), glm::vec3(0.0F)) << "nothing valid was received";
    EXPECT_FALSE(std::isnan(t.world[0][0]));
}

TEST(NetReplicaClient, ReplicatedDynamicBodiesBecomeKinematic) {
    entt::registry r;
    const auto body = [&](s::BodyMotion motion, bool replicated) {
        const entt::entity e = r.create();
        r.emplace<s::Transform>(e);
        r.emplace<s::RigidBody>(e, motion, 1.0F, 7U);
        if (replicated) r.emplace<s::NetReplicated>(e, 100U + entt::to_integral(e));
        return e;
    };
    const entt::entity dyn = body(s::BodyMotion::dynamic, true);
    const entt::entity stat = body(s::BodyMotion::static_body, true);
    const entt::entity local = body(s::BodyMotion::dynamic, false);

    EXPECT_EQ(engine::net::make_replicas_kinematic(r), 1U);
    EXPECT_EQ(r.get<s::RigidBody>(dyn).motion, s::BodyMotion::kinematic);
    EXPECT_EQ(r.get<s::RigidBody>(dyn).body, engine::physics::PhysicsWorld::invalid_body)
        << "the dynamic Jolt body is rebuilt as kinematic";
    EXPECT_EQ(r.get<s::RigidBody>(stat).motion, s::BodyMotion::static_body);
    EXPECT_EQ(r.get<s::RigidBody>(stat).body, 7U);
    EXPECT_EQ(r.get<s::RigidBody>(local).motion, s::BodyMotion::dynamic);
}

} // namespace
