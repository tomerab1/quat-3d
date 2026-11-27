// NetSession host hardening end to end over GNS loopback (S2, #68): raw
// clients against a hosting NetSession. Snapshots reach authenticated peers
// only, the player character goes to the first authenticated peer (never to
// the first input), silent peers are dropped at the auth deadline and
// flooding peers are dropped by the rate limit.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/net/host_access.hpp"
#include "engine/net/prediction.hpp"
#include "engine/net/session.hpp"
#include "engine/net/transport.hpp"
#include "engine/scene/components.hpp"
#include "net/loopback.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace net = engine::net;
namespace s = engine::scene;

constexpr float k_frame = 0.05F; // host frame: one 20 Hz snapshot per frame

struct Host {
    entt::registry                registry;
    entt::entity                  player = entt::null;
    std::optional<net::NetSession> session;

    [[nodiscard]] static std::optional<Host> start(net::HostConfig& config) {
        config.server.port = quat::test::next_loopback_port();
        std::optional<Host> host(std::in_place);
        // A replicated prop (named + Transform) and the player character.
        const entt::entity crate = host->registry.create();
        host->registry.emplace<s::Name>(crate, "crate");
        host->registry.emplace<s::Transform>(crate);
        host->player = host->registry.create();
        host->registry.emplace<s::Transform>(host->player);
        host->registry.emplace<s::CharacterController>(host->player);
        auto session = net::NetSession::host(host->registry, config);
        if (!session) {
            ADD_FAILURE() << session.error().message;
            return std::nullopt;
        }
        host->session.emplace(std::move(*session));
        return host;
    }

    void frame(float dt = k_frame) {
        session->update(registry, nullptr, player, dt);
        session->publish(registry, dt);
    }
};

// A raw client: optionally sends a hello as soon as it is connected, and
// records what it receives.
struct Peer {
    std::optional<net::NetClient> client;
    std::optional<std::string>    token; // hello to send once connected
    bool                          was_connected = false;
    std::optional<std::int32_t>   end_reason; // set once disconnected
    int                           snapshots = 0;

    [[nodiscard]] static std::optional<Peer> connect(std::uint16_t port,
                                                     std::optional<std::string> token) {
        auto c = net::NetClient::connect("127.0.0.1", port);
        if (!c) {
            ADD_FAILURE() << c.error().message;
            return std::nullopt;
        }
        Peer peer;
        peer.client.emplace(std::move(*c));
        peer.token = std::move(token);
        return peer;
    }

    void poll() {
        for (const net::NetEvent& ev : client->poll()) {
            if (ev.type == net::NetEvent::Type::connected) {
                was_connected = true;
                if (token) {
                    const auto hello = net::encode_hello(*token);
                    EXPECT_TRUE(hello && client->send(*hello, /*reliable=*/true));
                }
            } else if (ev.type == net::NetEvent::Type::disconnected) {
                end_reason = ev.end_reason;
            } else if (!ev.payload.empty() && ev.payload[0] == 1) { // snapshot kind
                ++snapshots;
            }
        }
    }
};

// Runs host frames, polling every peer, until `done` or ~4 s of wall time.
template <typename Done>
void run(Host& host, std::vector<Peer*> peers, Done done, float dt = k_frame) {
    for (int i = 0; i < 2000 && !done(); ++i) {
        host.frame(dt);
        for (Peer* p : peers) p->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

[[nodiscard]] std::int32_t reason(net::CloseReason r) { return static_cast<std::int32_t>(r); }

TEST(NetSessionHost, SnapshotsOnlyReachAuthenticatedPeers) {
    net::HostConfig config;
    config.access.join_token = "letmein";
    config.access.auth_timeout = 60.0F; // the silent peer stays for this test
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());

    auto player = Peer::connect(config.server.port, "letmein");
    auto silent = Peer::connect(config.server.port, std::nullopt);
    ASSERT_TRUE(player && silent);
    // Both connected and several snapshots out, then a few more frames, so
    // the silent peer has had every chance to get one.
    run(*host, {&*player, &*silent},
        [&] { return player->snapshots >= 5 && silent->was_connected; });
    int frames = 0;
    run(*host, {&*player, &*silent}, [&] { return ++frames > 10; });

    EXPECT_GE(player->snapshots, 5) << "the authenticated peer gets snapshots";
    EXPECT_TRUE(silent->was_connected);
    EXPECT_EQ(silent->snapshots, 0) << "an unauthenticated peer must get no snapshots";
    EXPECT_EQ(host->session->access().peer_count(), 2U);
    EXPECT_EQ(host->session->access().recipients().size(), 1U);
}

TEST(NetSessionHost, OwnershipGoesToTheFirstAuthenticatedPeerNotTheFirstInput) {
    net::HostConfig config;
    config.access.join_token = "letmein";
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());

    // An intruder that skips the hello and sends input straight away.
    auto intruder = Peer::connect(config.server.port, std::nullopt);
    ASSERT_TRUE(intruder.has_value());
    run(*host, {&*intruder}, [&] { return intruder->was_connected; });
    ASSERT_TRUE(intruder->was_connected);
    const auto input = net::encode_input({1U, glm::vec3(1.0F, 0.0F, 0.0F), k_frame});
    ASSERT_TRUE(intruder->client->send(input, /*reliable=*/true));
    run(*host, {&*intruder}, [&] { return intruder->end_reason.has_value(); });
    EXPECT_EQ(intruder->end_reason, reason(net::CloseReason::auth_failed));
    EXPECT_EQ(host->session->owner(), net::invalid_client);
    EXPECT_FALSE(host->registry.all_of<s::NetDriven>(host->player));

    // A wrong token gets nothing either.
    auto guesser = Peer::connect(config.server.port, "guess");
    ASSERT_TRUE(guesser.has_value());
    run(*host, {&*guesser}, [&] { return guesser->end_reason.has_value(); });
    EXPECT_EQ(guesser->end_reason, reason(net::CloseReason::auth_failed));
    EXPECT_EQ(host->session->owner(), net::invalid_client);

    // The first authenticated peer gets the character.
    auto player = Peer::connect(config.server.port, "letmein");
    ASSERT_TRUE(player.has_value());
    run(*host, {&*player}, [&] { return host->session->owner() != net::invalid_client; });
    const net::ClientId owner = host->session->owner();
    ASSERT_NE(owner, net::invalid_client);
    EXPECT_TRUE(host->session->access().authenticated(owner));
    host->frame();
    EXPECT_TRUE(host->registry.all_of<s::NetDriven>(host->player))
        << "the remote owner drives the character";
}

TEST(NetSessionHost, SilentPeersAreDroppedAtTheAuthDeadline) {
    net::HostConfig config;
    config.access.auth_timeout = 0.5F; // 10 host frames
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());

    auto silent = Peer::connect(config.server.port, std::nullopt);
    auto player = Peer::connect(config.server.port, "");
    ASSERT_TRUE(silent && player);
    run(*host, {&*silent, &*player}, [&] { return silent->end_reason.has_value(); });

    EXPECT_TRUE(silent->was_connected);
    EXPECT_EQ(silent->end_reason, reason(net::CloseReason::auth_timeout));
    EXPECT_FALSE(player->end_reason.has_value()) << "the authenticated peer stays";
    EXPECT_EQ(host->session->access().peer_count(), 1U);
}

TEST(NetSessionHost, FloodingPeersAreDropped) {
    net::HostConfig config;
    config.access.max_messages_per_second = 50; // burst: 100 messages
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());

    auto flooder = Peer::connect(config.server.port, "");
    ASSERT_TRUE(flooder.has_value());
    run(*host, {&*flooder},
        [&] { return host->session->access().recipients().size() == 1U; });
    ASSERT_EQ(host->session->access().recipients().size(), 1U);

    for (std::uint32_t seq = 1; seq <= 400; ++seq) {
        const auto input = net::encode_input({seq, glm::vec3(1.0F, 0.0F, 0.0F), k_frame});
        ASSERT_TRUE(flooder->client->send(input, /*reliable=*/true));
    }
    run(*host, {&*flooder}, [&] { return flooder->end_reason.has_value(); }, 1.0F / 60.0F);
    EXPECT_EQ(flooder->end_reason, reason(net::CloseReason::rate_limited));
    EXPECT_EQ(host->session->owner(), net::invalid_client)
        << "the dropped owner lost the character";
    EXPECT_EQ(host->session->access().peer_count(), 0U);
}

} // namespace
