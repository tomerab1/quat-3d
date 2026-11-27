// Snapshot building and client-side interpolation — transport-free. The
// broadcast step and the GNS loopback self-test are in replication_transport.cpp.

#include "engine/net/replication.hpp"

#include <cmath>
#include <cstring>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/scene/components.hpp"

namespace engine::net {

namespace {

constexpr std::uint8_t kSnapshotKind = 1;
constexpr std::size_t kEntryBytes = snapshot_entry_bytes;
constexpr float kInterpWindow = 0.1F; // seconds to reach a snapshot

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
    if (payload.size() < 3 || payload[0] != kSnapshotKind) return;
    std::uint16_t count = 0;
    std::memcpy(&count, payload.data() + 1, 2);
    if (payload.size() < 3 + static_cast<std::size_t>(count) * kEntryBytes) return;

    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* at = payload.data() + 3 + static_cast<std::size_t>(i) * kEntryBytes;
        std::uint32_t id = 0;
        Target target;
        std::memcpy(&id, at, 4);
        std::memcpy(&target.position, at + 4, 12);
        std::memcpy(&target.rotation, at + 16, 16);
        target.fresh = true;
        targets_[id] = target;
    }
}

void ReplicationClient::apply(entt::registry& registry, float dt) {
    if (targets_.empty()) return;
    const float alpha = glm::clamp(dt / kInterpWindow, 0.0F, 1.0F);

    for (auto [e, rep, t] : registry.view<scene::NetReplicated, scene::Transform>().each()) {
        const auto it = targets_.find(rep.net_id);
        if (it == targets_.end()) continue;
        const Target& target = it->second;

        const glm::vec3 pos = glm::mix(glm::vec3(t.local[3]), target.position, alpha);
        glm::mat3 rot_m(t.local);
        rot_m[0] = glm::normalize(rot_m[0]);
        rot_m[1] = glm::normalize(rot_m[1]);
        rot_m[2] = glm::normalize(rot_m[2]);
        const glm::quat rot = glm::slerp(glm::quat_cast(rot_m), target.rotation, alpha);

        // Replicated entities are simulated server-side as world-space roots.
        const glm::vec3 scale(glm::length(glm::vec3(t.local[0])),
                              glm::length(glm::vec3(t.local[1])),
                              glm::length(glm::vec3(t.local[2])));
        glm::mat4 m = glm::mat4_cast(rot);
        m[0] *= scale.x;
        m[1] *= scale.y;
        m[2] *= scale.z;
        m[3] = glm::vec4(pos, 1.0F);
        t.local = m;
        t.world = m;
    }
}

} // namespace engine::net
