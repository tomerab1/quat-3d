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
// A snapshot is split into self-contained packets of at most
// max_snapshot_packet_bytes (below the path MTU, so an unreliable packet is
// one datagram and a loss costs only its own entries); each packet is applied
// independently, so no reassembly is needed.

#include <cstddef>
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

inline constexpr std::size_t snapshot_header_bytes = 1 + 2;          // kind + count
inline constexpr std::size_t snapshot_entry_bytes = 4 + 12 + 16;     // id + pos + quat
inline constexpr std::size_t max_snapshot_packet_bytes = 1100;       // < ~1200 B MTU payload
inline constexpr std::size_t max_snapshot_entries =
    (max_snapshot_packet_bytes - snapshot_header_bytes) / snapshot_entry_bytes;

struct SnapshotPacket {
    std::vector<std::uint8_t> bytes;
    bool                      reliable = false; // keyframes ride the reliable channel
};

// Server: track which entities changed and broadcast snapshots at `rate_hz`.
class ReplicationServer {
public:
    explicit ReplicationServer(float rate_hz = 20.0F)
        : interval_(rate_hz > 0.0F ? 1.0F / rate_hz : 0.05F) {}

    // Advance the snapshot clock by `dt`. When a snapshot is due, returns its
    // packets (empty otherwise, or when nothing moved). At most one snapshot
    // per call; the accumulator keeps its remainder so the long-run rate is
    // exact, but a hitch does not trigger a burst of back-to-back snapshots.
    [[nodiscard]] std::vector<SnapshotPacket> collect(entt::registry& registry, float dt);

    // collect() + broadcast. A packet the transport refuses is not lost: its
    // entities are forgotten from the delta baseline and go out again in the
    // next snapshot. Call once per frame (the caller owns server.poll()).
    void tick(entt::registry& registry, NetServer& server, float dt);

    // Packets the transport refused since start (logged on the first and then
    // every 100th failure).
    [[nodiscard]] std::uint32_t send_failures() const { return send_failures_; }

    // Snapshots between reliable keyframes (~1.5 s at 20 Hz).
    static constexpr int keyframe_interval = 30;

private:
    void forget(const SnapshotPacket& packet);

    struct Sent {
        glm::vec3 position{0.0F};
        glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    };
    float         interval_ = 0.05F;
    float         accumulator_ = 0.0F;
    int           snapshots_since_keyframe_ = keyframe_interval; // first snapshot is a keyframe
    std::uint32_t send_failures_ = 0;
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
