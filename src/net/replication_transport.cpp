// Replication over the GNS transport: the server's broadcast step. The
// snapshot / interpolation logic itself lives in replication.cpp and is
// transport-free (unit-testable without sockets).

#include "engine/net/replication.hpp"

#include <cstdio>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"

namespace engine::net {

void ReplicationServer::tick(entt::registry& registry, NetServer& server, float dt) {
    for (const SnapshotPacket& packet : collect(registry, dt)) {
        if (server.broadcast(packet.bytes, packet.reliable)) continue;
        // Refused (send queue full, connection closing): resend these
        // entities with the next snapshot instead of waiting for a keyframe.
        forget(packet);
        if (send_failures_++ % 100 == 0) {
            std::fprintf(stderr,
                         "[net] snapshot packet (%zu B, %s) refused by the transport "
                         "(%u failures so far)\n",
                         packet.bytes.size(), packet.reliable ? "reliable" : "unreliable",
                         send_failures_);
        }
    }
}

} // namespace engine::net
