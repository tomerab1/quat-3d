// Snapshot building and client-side interpolation — transport-free. The
// broadcast step and the GNS loopback self-test are in replication_transport.cpp.

#include "engine/net/replication.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"

namespace engine::net {

namespace {

constexpr std::uint8_t kSnapshotKind = 1;
constexpr std::size_t kEntryBytes = snapshot_entry_bytes;
constexpr int k_max_depth = 256; // hierarchy cycles are rejected upstream; belt and braces

[[nodiscard]] bool all_finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
[[nodiscard]] bool all_finite(const glm::quat& q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

// Scale-free rotation of an affine matrix.
[[nodiscard]] glm::quat rotation_of(const glm::mat4& m) {
    glm::mat3 r(m);
    r[0] = glm::normalize(r[0]);
    r[1] = glm::normalize(r[1]);
    r[2] = glm::normalize(r[2]);
    return glm::quat_cast(r);
}

[[nodiscard]] entt::entity parent_of(const entt::registry& registry, entt::entity e) {
    const auto* p = registry.try_get<scene::Parent>(e);
    return p != nullptr && p->entity != entt::null && registry.valid(p->entity) &&
                   registry.all_of<scene::Transform>(p->entity)
               ? p->entity
               : entt::entity{entt::null};
}

// World matrix of `e` composed from the current locals up the hierarchy —
// fresh even when an ancestor's local changed this frame (Transform.world is
// only recomputed by the next transform_system).
[[nodiscard]] glm::mat4 fresh_world(const entt::registry& registry, entt::entity e) {
    glm::mat4 world = registry.get<scene::Transform>(e).local;
    int depth = 0;
    for (entt::entity p = parent_of(registry, e); p != entt::null && depth < k_max_depth;
         p = parent_of(registry, p), ++depth) {
        world = registry.get<scene::Transform>(p).local * world;
    }
    return world;
}

static_assert(max_snapshot_entries > 0 && max_snapshot_entries <= 0xFFFF,
              "snapshot packet count is a u16");

void write_entry(std::vector<std::uint8_t>& out, std::uint32_t id, const glm::vec3& p,
                 const glm::quat& q) {
    const std::size_t at = out.size();
    out.resize(at + kEntryBytes);
    std::memcpy(out.data() + at, &id, 4);
    std::memcpy(out.data() + at + 4, &p, 12);
    std::memcpy(out.data() + at + 16, &q, 16);
}

void begin_packet(std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(max_snapshot_packet_bytes);
    out.push_back(kSnapshotKind);
    out.push_back(0); // count, patched by finish_packet
    out.push_back(0);
}

void finish_packet(std::vector<std::uint8_t>& out) {
    const auto count =
        static_cast<std::uint16_t>((out.size() - snapshot_header_bytes) / kEntryBytes);
    std::memcpy(out.data() + 1, &count, 2);
}

} // namespace

std::vector<SnapshotPacket> ReplicationServer::collect(entt::registry& registry, float dt) {
    std::vector<SnapshotPacket> packets;
    if (!(dt > 0.0F) || !std::isfinite(dt)) return packets;
    accumulator_ += dt;
    if (accumulator_ < interval_) return packets;
    // Keep the remainder (an exact long-run rate); after a hitch, drop whole
    // intervals rather than sending a burst of identical snapshots.
    accumulator_ = std::fmod(accumulator_ - interval_, interval_);

    const bool keyframe = snapshots_since_keyframe_ >= keyframe_interval;
    snapshots_since_keyframe_ = keyframe ? 0 : snapshots_since_keyframe_ + 1;

    SnapshotPacket current;
    current.reliable = keyframe;
    begin_packet(current.bytes);
    for (auto [e, rep, t] :
         registry.view<const scene::NetReplicated, const scene::Transform>().each()) {
        if (rep.net_id == 0) continue;
        const glm::vec3 pos(t.world[3]);
        glm::mat3 rot_m(t.world);
        rot_m[0] = glm::normalize(rot_m[0]);
        rot_m[1] = glm::normalize(rot_m[1]);
        rot_m[2] = glm::normalize(rot_m[2]);
        const glm::quat rot = glm::quat_cast(rot_m);

        const auto it = last_sent_.find(rep.net_id);
        const bool moved = it == last_sent_.end() ||
                           glm::distance(it->second.position, pos) > 1e-4F ||
                           glm::abs(glm::dot(it->second.rotation, rot)) < 1.0F - 1e-6F;
        if (!keyframe && !moved) continue;

        if (current.bytes.size() + kEntryBytes > max_snapshot_packet_bytes) {
            finish_packet(current.bytes);
            packets.push_back(std::move(current));
            current = SnapshotPacket{};
            current.reliable = keyframe;
            begin_packet(current.bytes);
        }
        write_entry(current.bytes, rep.net_id, pos, rot);
        last_sent_[rep.net_id] = {pos, rot};
    }
    if (current.bytes.size() > snapshot_header_bytes) {
        finish_packet(current.bytes);
        packets.push_back(std::move(current));
    }
    return packets;
}

void ReplicationServer::forget(const SnapshotPacket& packet) {
    for (std::size_t at = snapshot_header_bytes; at + kEntryBytes <= packet.bytes.size();
         at += kEntryBytes) {
        std::uint32_t id = 0;
        std::memcpy(&id, packet.bytes.data() + at, 4);
        last_sent_.erase(id);
    }
}

void ReplicationClient::receive(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < snapshot_header_bytes || payload[0] != kSnapshotKind) return;
    std::uint16_t count = 0;
    std::memcpy(&count, payload.data() + 1, 2);
    if (payload.size() != snapshot_header_bytes + static_cast<std::size_t>(count) * kEntryBytes) {
        return;
    }

    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* at =
            payload.data() + snapshot_header_bytes + static_cast<std::size_t>(i) * kEntryBytes;
        std::uint32_t id = 0;
        glm::vec3 position(0.0F);
        glm::quat rotation(1.0F, 0.0F, 0.0F, 0.0F);
        std::memcpy(&id, at, 4);
        std::memcpy(&position, at + 4, 12);
        std::memcpy(&rotation, at + 16, 16);
        if (id == 0 || !all_finite(position) || !all_finite(rotation)) continue;
        const float len = glm::length(rotation);
        if (!(len > 1e-6F)) continue;

        Target& target = targets_[id];
        target.position = position;
        target.rotation = rotation / len;
    }
}

void ReplicationClient::apply(entt::registry& registry, float dt) {
    if (targets_.empty()) return;
    // Exponential smoothing: frame-rate independent, ~95 % of the way to the
    // snapshot after interpolation_window.
    const float alpha =
        dt > 0.0F && std::isfinite(dt) ? 1.0F - std::exp(-3.0F * dt / interpolation_window)
                                       : 0.0F;

    // Parents before children: a child's local is derived from its parent's
    // pose, which may itself be replicated this frame.
    struct Item {
        entt::entity e;
        int          depth;
        Target*      target;
    };
    std::vector<Item> items;
    for (auto [e, rep, t] :
         registry.view<const scene::NetReplicated, const scene::Transform>().each()) {
        const auto it = targets_.find(rep.net_id);
        if (it == targets_.end()) continue;
        int depth = 0;
        for (entt::entity p = parent_of(registry, e); p != entt::null && depth < k_max_depth;
             p = parent_of(registry, p)) {
            ++depth;
        }
        items.push_back({e, depth, &it->second});
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.depth < b.depth; });

    for (const Item& item : items) {
        auto& t = registry.get<scene::Transform>(item.e);
        Target& target = *item.target;
        if (!target.has_smoothed) {
            target.smoothed_position = glm::vec3(t.world[3]);
            target.smoothed_rotation = rotation_of(t.world);
            target.has_smoothed = true;
        }
        target.smoothed_position = glm::mix(target.smoothed_position, target.position, alpha);
        target.smoothed_rotation =
            glm::normalize(glm::slerp(target.smoothed_rotation, target.rotation, alpha));

        // Keep the entity's authored world scale; snapshots carry only pose.
        const glm::vec3 scale(glm::length(glm::vec3(t.world[0])),
                              glm::length(glm::vec3(t.world[1])),
                              glm::length(glm::vec3(t.world[2])));
        glm::mat4 world = glm::mat4_cast(target.smoothed_rotation);
        world[0] *= scale.x;
        world[1] *= scale.y;
        world[2] *= scale.z;
        world[3] = glm::vec4(target.smoothed_position, 1.0F);

        const entt::entity parent = parent_of(registry, item.e);
        t.local = parent == entt::null ? world
                                       : glm::inverse(fresh_world(registry, parent)) * world;
        t.world = world;
    }
}

std::uint32_t make_replicas_kinematic(entt::registry& registry) {
    std::uint32_t changed = 0;
    for (auto [e, rep, rb] : registry.view<const scene::NetReplicated, scene::RigidBody>().each()) {
        if (rb.motion != scene::BodyMotion::dynamic) continue;
        rb.motion = scene::BodyMotion::kinematic;
        // An existing dynamic body is swept by physics_system (its RigidBody
        // no longer names it) and rebuilt as kinematic.
        rb.body = physics::PhysicsWorld::invalid_body;
        ++changed;
    }
    return changed;
}

} // namespace engine::net
