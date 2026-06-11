#include "engine/net/replication.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/scene/components.hpp"

namespace engine::net {

namespace {

constexpr std::uint8_t kSnapshotKind = 1;
constexpr std::size_t kEntryBytes = 4 + 12 + 16; // id + pos + quat
constexpr float kInterpWindow = 0.1F;            // seconds to reach a snapshot

void write_entry(std::vector<std::uint8_t>& out, std::uint32_t id, const glm::vec3& p,
                 const glm::quat& q) {
    const std::size_t at = out.size();
    out.resize(at + kEntryBytes);
    std::memcpy(out.data() + at, &id, 4);
    std::memcpy(out.data() + at + 4, &p, 12);
    std::memcpy(out.data() + at + 16, &q, 16);
}

} // namespace

void ReplicationServer::tick(entt::registry& registry, NetServer& server, float dt) {
    accumulator_ += dt;
    if (accumulator_ < interval_) return;
    accumulator_ = 0.0F;

    const bool keyframe = snapshots_since_keyframe_ >= 30; // ~1.5 s at 20 Hz
    snapshots_since_keyframe_ = keyframe ? 0 : snapshots_since_keyframe_ + 1;

    std::vector<std::uint8_t> packet;
    packet.reserve(3 + 16 * kEntryBytes);
    packet.push_back(kSnapshotKind);
    packet.push_back(0); // count, patched below
    packet.push_back(0);

    std::uint16_t count = 0;
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

        write_entry(packet, rep.net_id, pos, rot);
        last_sent_[rep.net_id] = {pos, rot};
        ++count;
    }
    if (count == 0) return;
    std::memcpy(packet.data() + 1, &count, 2);
    // Keyframes ride the reliable channel (joiners, loss recovery); the steady
    // stream is unreliable latest-wins.
    server.broadcast(packet, /*reliable=*/keyframe);
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

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error> run_replication_self_test() {
    const auto fail = [](const char* what) {
        return std::unexpected(core::Error{std::string(what)});
    };

    auto server = NetServer::listen(42778);
    if (!server) return std::unexpected(server.error());
    auto client = NetClient::connect("127.0.0.1", 42778);
    if (!client) return std::unexpected(client.error());

    // Server world: one replicated entity on the move.
    entt::registry server_reg;
    const entt::entity se = server_reg.create();
    server_reg.emplace<scene::NetReplicated>(se, 7U);
    auto& st = server_reg.emplace<scene::Transform>(se);

    // Client world: the matching placeholder at the origin.
    entt::registry client_reg;
    const entt::entity ce = client_reg.create();
    client_reg.emplace<scene::NetReplicated>(ce, 7U);
    client_reg.emplace<scene::Transform>(ce);

    ReplicationServer replication(60.0F); // fast for the test
    ReplicationClient receiver;

    const glm::vec3 destination(12.0F, 3.0F, -4.0F);
    bool connected = false;
    for (int i = 0; i < 1500; ++i) {
        const float dt = 1.0F / 120.0F;
        // Server: glide the entity toward the destination and replicate.
        const glm::vec3 cur(st.world[3]);
        const glm::vec3 next = glm::mix(cur, destination, 0.05F);
        st.local[3] = glm::vec4(next, 1.0F);
        st.world = st.local;
        for (const NetEvent& ev : server->poll()) {
            connected = connected || ev.type == NetEvent::Type::connected;
        }
        replication.tick(server_reg, *server, dt);

        for (const NetEvent& ev : client->poll()) {
            if (ev.type == NetEvent::Type::message) receiver.receive(ev.payload);
        }
        receiver.apply(client_reg, dt);

        const glm::vec3 mirrored(client_reg.get<scene::Transform>(ce).world[3]);
        if (connected && glm::distance(mirrored, destination) < 0.05F) {
            return {};
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return fail("replication self-test: client transform never converged on the server's");
}

} // namespace engine::net
