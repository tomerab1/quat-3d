// Snapshot building (S1 audit B15): chunking below the MTU, exact send rate,
// keyframes. Transport-free — the GNS loopback path is Net.Replication.

#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/net/replication.hpp"
#include "engine/scene/components.hpp"

namespace {

namespace s = engine::scene;
using engine::net::ReplicationClient;
using engine::net::ReplicationServer;
using engine::net::SnapshotPacket;

std::vector<entt::entity> spawn(entt::registry& r, std::uint32_t count) {
    std::vector<entt::entity> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const entt::entity e = r.create();
        r.emplace<s::NetReplicated>(e, i + 1);
        auto& t = r.emplace<s::Transform>(e);
        t.local = glm::translate(glm::mat4(1.0F), glm::vec3(static_cast<float>(i), 0.0F, 0.0F));
        t.world = t.local;
        out.push_back(e);
    }
    return out;
}

std::uint16_t count_of(const SnapshotPacket& p) {
    std::uint16_t count = 0;
    std::memcpy(&count, p.bytes.data() + 1, 2);
    return count;
}

TEST(NetSnapshot, LargeSnapshotsAreChunkedBelowTheMtu) {
    entt::registry r;
    constexpr std::uint32_t n = 3000; // > u16-safe single-packet sizes and > 64 KiB
    (void)spawn(r, n);
    ReplicationServer server(20.0F);
    const std::vector<SnapshotPacket> packets = server.collect(r, 0.05F);

    ASSERT_GT(packets.size(), 1U);
    std::unordered_map<std::uint32_t, int> seen;
    for (const SnapshotPacket& p : packets) {
        EXPECT_TRUE(p.reliable) << "the first snapshot is a keyframe";
        EXPECT_LE(p.bytes.size(), engine::net::max_snapshot_packet_bytes);
        const std::uint16_t count = count_of(p);
        EXPECT_LE(count, engine::net::max_snapshot_entries);
        ASSERT_EQ(p.bytes.size(), engine::net::snapshot_header_bytes +
                                      count * engine::net::snapshot_entry_bytes);
        for (std::uint16_t i = 0; i < count; ++i) {
            std::uint32_t id = 0;
            std::memcpy(&id,
                        p.bytes.data() + engine::net::snapshot_header_bytes +
                            static_cast<std::size_t>(i) * engine::net::snapshot_entry_bytes,
                        4);
            ++seen[id];
        }
    }
    ASSERT_EQ(seen.size(), n);
    for (const auto& [id, times] : seen) EXPECT_EQ(times, 1) << "net_id " << id;

    // Every chunk decodes on the client independently.
    entt::registry client;
    const std::vector<entt::entity> mirrors = spawn(client, n);
    for (const entt::entity e : mirrors) {
        client.get<s::Transform>(e).local = glm::mat4(1.0F);
        client.get<s::Transform>(e).world = glm::mat4(1.0F);
    }
    ReplicationClient receiver;
    for (const SnapshotPacket& p : packets) receiver.receive(p.bytes);
    for (int i = 0; i < 100; ++i) receiver.apply(client, 1.0F / 60.0F);
    EXPECT_NEAR(client.get<s::Transform>(mirrors.back()).world[3].x,
                static_cast<float>(n - 1), 1e-2F);
}

TEST(NetSnapshot, OnlyMovedEntitiesBetweenKeyframes) {
    entt::registry r;
    const std::vector<entt::entity> es = spawn(r, 10);
    ReplicationServer server(20.0F);
    ASSERT_FALSE(server.collect(r, 0.05F).empty()); // keyframe

    EXPECT_TRUE(server.collect(r, 0.05F).empty()) << "nothing moved";
    r.get<s::Transform>(es[3]).world[3].y = 2.0F;
    const std::vector<SnapshotPacket> delta = server.collect(r, 0.05F);
    ASSERT_EQ(delta.size(), 1U);
    EXPECT_FALSE(delta[0].reliable);
    EXPECT_EQ(count_of(delta[0]), 1U);
}

TEST(NetSnapshot, AccumulatorKeepsTheRemainder) {
    entt::registry r;
    const std::vector<entt::entity> es = spawn(r, 1);
    ReplicationServer server(20.0F); // every 0.05 s
    int snapshots = 0;
    // 0.03 s frames for 3.33 s: 66 snapshots. Resetting the accumulator to zero
    // (the old behaviour) sends one every 0.06 s — 55.
    for (int frame = 0; frame < 111; ++frame) {
        r.get<s::Transform>(es[0]).world[3].x = static_cast<float>(frame); // always moving
        snapshots += server.collect(r, 0.03F).empty() ? 0 : 1;
    }
    EXPECT_EQ(snapshots, 66);
}

TEST(NetSnapshot, HitchSendsOneSnapshotNotABurst) {
    entt::registry r;
    (void)spawn(r, 1);
    ReplicationServer server(20.0F);
    EXPECT_FALSE(server.collect(r, 1.0F).empty());
    // The 1 s hitch is not paid back with 19 more snapshots.
    EXPECT_TRUE(server.collect(r, 0.001F).empty());
}

TEST(NetSnapshot, IgnoresInvalidTime) {
    entt::registry r;
    (void)spawn(r, 1);
    ReplicationServer server(20.0F);
    EXPECT_TRUE(server.collect(r, -1.0F).empty());
    EXPECT_TRUE(server.collect(r, std::numeric_limits<float>::quiet_NaN()).empty());
    EXPECT_FALSE(server.collect(r, 0.05F).empty());
}

} // namespace
