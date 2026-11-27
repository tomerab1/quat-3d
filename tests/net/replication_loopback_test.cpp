// Replication end to end over the GNS loopback transport: the server's
// ReplicationServer::tick broadcasts snapshots of a moving entity and the
// client's ReplicationClient mirrors it onto its matching placeholder.

#include <chrono>
#include <thread>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/net/replication.hpp"
#include "engine/net/transport.hpp"
#include "engine/scene/components.hpp"
#include "net/loopback.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::net::NetEvent;

TEST(NetReplicationLoopback, ClientTransformConvergesOnTheServers) {
    quat::test::LoopbackPair pair;
    ASSERT_OK(pair.open());
    ASSERT_TRUE(pair.handshake()) << "loopback handshake never completed";

    // Server world: one replicated entity on the move.
    entt::registry     server_reg;
    const entt::entity se = server_reg.create();
    server_reg.emplace<s::NetReplicated>(se, 7U);
    auto& st = server_reg.emplace<s::Transform>(se);

    // Client world: the matching placeholder at the origin.
    entt::registry     client_reg;
    const entt::entity ce = client_reg.create();
    client_reg.emplace<s::NetReplicated>(ce, 7U);
    client_reg.emplace<s::Transform>(ce);

    engine::net::ReplicationServer replication(60.0F); // fast for the test
    engine::net::ReplicationClient receiver;

    const glm::vec3 destination(12.0F, 3.0F, -4.0F);
    float           distance = glm::distance(glm::vec3(0.0F), destination);
    for (int i = 0; i < 1500 && distance >= 0.05F; ++i) {
        constexpr float dt = 1.0F / 120.0F;
        // Server: glide the entity toward the destination and replicate.
        const glm::vec3 next = glm::mix(glm::vec3(st.world[3]), destination, 0.05F);
        st.local[3] = glm::vec4(next, 1.0F);
        st.world = st.local;
        (void)pair.server->poll();
        replication.tick(server_reg, *pair.server, dt);

        for (const NetEvent& ev : pair.client->poll()) {
            if (ev.type == NetEvent::Type::message) receiver.receive(ev.payload);
        }
        receiver.apply(client_reg, dt);

        distance = glm::distance(glm::vec3(client_reg.get<s::Transform>(ce).world[3]), destination);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    EXPECT_LT(distance, 0.05F) << "client transform never converged on the server's";
}

} // namespace
