#pragma once

// Replication (Phase 14, Slice 14.2) — server-authoritative state snapshots.
//
// Entities carrying NetReplicated mirror their Transform from the server to
// every client. The server snapshots at a fixed rate, sending only entities
// that moved since the previous snapshot (delta'd against the last sent
// state) as unreliable messages, plus a periodic reliable keyframe carrying
// everything (joiners, packet loss). Clients smooth toward the received state
// over an interpolation window instead of snapping. Both ends must share the
// scene (same scene file / construction): replication matches entities by
// net_id, it does not spawn meshes.
//
// Wire format (little-endian, both targets are x86_64):
//   [u8 kind: 1 = snapshot] [u16 count] count * { u32 net_id, 3f pos, 4f quat }

#include <cstdint>
#include <expected>
#include <unordered_map>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/error.hpp"
#include "engine/net/transport.hpp"

namespace engine::net {

// Server: track which entities changed and broadcast snapshots at `rate_hz`.
class ReplicationServer {
public:
    explicit ReplicationServer(float rate_hz = 20.0F) : interval_(1.0F / rate_hz) {}

    // Call once per frame; drains nothing (the caller owns server.poll()).
    void tick(entt::registry& registry, NetServer& server, float dt);

private:
    struct Sent {
        glm::vec3 position{0.0F};
        glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    };
    float interval_ = 0.05F;
    float accumulator_ = 0.0F;
    int   snapshots_since_keyframe_ = 0;
    std::unordered_map<std::uint32_t, Sent> last_sent_;
};

// Client: apply incoming snapshots and smooth entities toward them.
class ReplicationClient {
public:
    // Feed every transport message payload here; non-snapshot kinds are ignored.
    void receive(const std::vector<std::uint8_t>& payload);

    // Smooth replicated entities toward their latest snapshot. `dt` drives the
    // interpolation (window ~0.1 s — one to two snapshot intervals).
    void apply(entt::registry& registry, float dt);

private:
    struct Target {
        glm::vec3 position{0.0F};
        glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
        bool      fresh = false;
    };
    std::unordered_map<std::uint32_t, Target> targets_;
};

// Loopback self-test: a moving server entity replicates to a client registry
// over real GNS loopback; the client copy converges on the server transform.
[[nodiscard]] std::expected<void, core::Error> run_replication_self_test();

} // namespace engine::net
