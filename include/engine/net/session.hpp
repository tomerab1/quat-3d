#pragma once

// NetSession — the game's networking glue (Phase 14 + S1 audit B1-B5).
//
// QUAT_HOST=<port> hosts the running scene; QUAT_JOIN=<ip:port> joins a host
// running the same scene. The host is authoritative: it simulates the remote
// player's inputs on the shared "player" character, one character step per
// input, acknowledges only inputs it has simulated, and broadcasts replicated
// transforms. The joined client predicts its character from local input and
// reconciles against the host's acknowledged state, and mirrors everything
// else from snapshots.
//
// Per frame (main thread), in this order:
//   drive_character()   local input -> character (offline / host / predicted)
//   update()            before the physics systems
//   ... physics systems, scene.tick() ...
//   publish()           after scene.tick() (fresh world transforms)

#include <cstdint>
#include <optional>
#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/net/prediction.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/transport.hpp"

namespace engine::physics {
class PhysicsWorld;
}

namespace engine::net {

class NetSession {
public:
    // Starts hosting / joining from QUAT_HOST / QUAT_JOIN (inactive when
    // neither is set or the transport failed — logged) and tags the scene's
    // replicated entities.
    [[nodiscard]] static NetSession from_environment(entt::registry& registry);

    NetSession();
    NetSession(NetSession&&) noexcept;
    NetSession& operator=(NetSession&&) noexcept;
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;
    ~NetSession();

    [[nodiscard]] bool active() const { return server_.has_value() || client_.has_value(); }
    [[nodiscard]] bool hosting() const { return server_.has_value(); }
    [[nodiscard]] bool joined() const { return client_.has_value(); }

    // The local player's wish direction for `character` this frame (xz,
    // |move| <= 1). Offline, or hosting with no remote player: sets the
    // controller's move. Hosting while a remote player owns the character:
    // ignored (the remote inputs drive it). Joined: sent to the host and
    // predicted locally.
    void drive_character(entt::registry& registry, physics::PhysicsWorld* world,
                         entt::entity character, const glm::vec3& move, float dt);

    // Pump the transport. Host: queue the remote player's inputs, simulate
    // the ones due this frame on `character` and acknowledge them. Client:
    // reconcile the predicted character and apply replicated transforms.
    // Call before the physics systems.
    void update(entt::registry& registry, physics::PhysicsWorld* world, entt::entity character,
                float dt);

    // Host: broadcast replication snapshots. Call after scene.tick().
    void publish(entt::registry& registry, float dt);

private:
    void host_update(entt::registry& registry, physics::PhysicsWorld* world,
                     entt::entity character, float dt);
    void client_update(entt::registry& registry, physics::PhysicsWorld* world,
                       entt::entity character, float dt);
    void release_character(entt::registry& registry, entt::entity character);

    std::optional<NetServer> server_;
    std::optional<NetClient> client_;
    ReplicationServer        replication_;
    ReplicationClient        receiver_;

    // Host: the remote player that owns the character (v1: one remote player).
    ClientId      owner_ = invalid_client;
    InputQueue    inputs_;
    std::uint32_t rejected_inputs_ = 0;

    // Client: prediction state.
    CharacterPredictor predictor_;
    std::uint32_t      next_sequence_ = 0;
};

} // namespace engine::net
