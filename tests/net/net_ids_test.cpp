// Replication id assignment (S1 audit B3): ids are unique even for entities
// that share a name, and identical on both ends of a shared scene.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/net/net_ids.hpp"
#include "engine/scene/components.hpp"

namespace {

namespace s = engine::scene;
using engine::net::assign_net_ids;

entt::entity add(entt::registry& r, const std::string& name, entt::entity parent = entt::null) {
    const entt::entity e = r.create();
    r.emplace<s::Transform>(e);
    r.emplace<s::Name>(e, name);
    if (parent != entt::null) r.emplace<s::Parent>(e, parent);
    return e;
}

// The physics demo's layout: a floor, a dozen identically named boxes, a
// player character, and a couple of identically named children.
std::vector<entt::entity> build_demo(entt::registry& r) {
    std::vector<entt::entity> out;
    out.push_back(add(r, "floor"));
    for (int i = 0; i < 12; ++i) out.push_back(add(r, "box"));
    const entt::entity player = add(r, "player");
    r.emplace<s::CharacterController>(player);
    out.push_back(player);
    const entt::entity rack_a = add(r, "rack");
    const entt::entity rack_b = add(r, "rack");
    out.push_back(rack_a);
    out.push_back(rack_b);
    out.push_back(add(r, "box", rack_a)); // same name, different path
    out.push_back(add(r, "box", rack_b)); // same path as the previous one
    return out;
}

std::uint32_t id_of(const entt::registry& r, entt::entity e) {
    const auto* rep = r.try_get<s::NetReplicated>(e);
    return rep != nullptr ? rep->net_id : 0U;
}

TEST(NetIds, DuplicateNamesGetUniqueIds) {
    entt::registry r;
    const std::vector<entt::entity> entities = build_demo(r);
    const engine::net::NetIdReport report = assign_net_ids(r);

    std::unordered_set<std::uint32_t> ids;
    std::size_t tagged = 0;
    for (const entt::entity e : entities) {
        if (r.all_of<s::CharacterController>(e)) {
            EXPECT_FALSE(r.all_of<s::NetReplicated>(e)) << "predicted character replicated";
            continue;
        }
        const std::uint32_t id = id_of(r, e);
        EXPECT_NE(id, 0U);
        EXPECT_TRUE(ids.insert(id).second) << "net_id " << id << " collides";
        ++tagged;
    }
    EXPECT_EQ(report.tagged, tagged);
    // 11 repeated top-level boxes, 1 repeated rack, 1 repeated rack/box.
    EXPECT_EQ(report.duplicate_names, 13U);
}

TEST(NetIds, BothEndsOfASharedSceneAgree) {
    entt::registry host;
    entt::registry client;
    const std::vector<entt::entity> h = build_demo(host);
    const std::vector<entt::entity> c = build_demo(client);
    (void)assign_net_ids(host);
    (void)assign_net_ids(client);
    ASSERT_EQ(h.size(), c.size());
    for (std::size_t i = 0; i < h.size(); ++i) {
        EXPECT_EQ(id_of(host, h[i]), id_of(client, c[i])) << "entity " << i;
    }
}

TEST(NetIds, PathAndOccurrenceFormTheKey) {
    entt::registry r;
    const entt::entity rack = add(r, "rack");
    const entt::entity box = add(r, "box", rack);
    EXPECT_EQ(engine::net::net_path(r, box), "rack/box");
    (void)assign_net_ids(r);
    EXPECT_EQ(id_of(r, box), engine::net::net_id_hash("rack/box#0"));
    EXPECT_EQ(id_of(r, rack), engine::net::net_id_hash("rack#0"));
}

TEST(NetIds, AuthoredIdsAreKeptAndDuplicatesReassigned) {
    entt::registry r;
    const entt::entity a = add(r, "a");
    const entt::entity b = add(r, "b");
    const entt::entity c = add(r, "c");
    r.emplace<s::NetReplicated>(a, 42U);
    r.emplace<s::NetReplicated>(b, 42U); // authored duplicate
    r.emplace<s::NetReplicated>(c, 0U);  // replicated, id unassigned
    const engine::net::NetIdReport report = assign_net_ids(r);

    EXPECT_EQ(id_of(r, a), 42U);
    EXPECT_NE(id_of(r, b), 42U);
    EXPECT_NE(id_of(r, b), 0U);
    EXPECT_NE(id_of(r, c), 0U);
    EXPECT_NE(id_of(r, b), id_of(r, c));
    EXPECT_EQ(report.kept, 1U);
    EXPECT_EQ(report.duplicate_ids, 1U);
    EXPECT_EQ(report.tagged, 2U);

    // Idempotent: a second pass keeps every id.
    const std::uint32_t before_b = id_of(r, b);
    const engine::net::NetIdReport again = assign_net_ids(r);
    EXPECT_EQ(again.tagged, 0U);
    EXPECT_EQ(id_of(r, b), before_b);
}

TEST(NetIds, HashCollisionsAreReprobed) {
    entt::registry r;
    // An authored id that happens to equal the hash of another entity's key.
    const entt::entity authored = add(r, "authored");
    r.emplace<s::NetReplicated>(authored, engine::net::net_id_hash("x#0"));
    const entt::entity x = add(r, "x");
    const engine::net::NetIdReport report = assign_net_ids(r);
    EXPECT_EQ(report.hash_collisions, 1U);
    EXPECT_NE(id_of(r, x), id_of(r, authored));
    EXPECT_NE(id_of(r, x), 0U);
}

TEST(NetIds, HashIsNeverZero) {
    EXPECT_NE(engine::net::net_id_hash(""), 0U);
    EXPECT_NE(engine::net::net_id_hash("box#0"), engine::net::net_id_hash("box#1"));
}

} // namespace
