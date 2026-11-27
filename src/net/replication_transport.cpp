// Replication over the GNS transport: the server's broadcast step and the
// loopback self-test. The snapshot / interpolation logic itself lives in
// replication.cpp and is transport-free (unit-testable without sockets).

#include "engine/net/replication.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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
