// NetSession host hardening end to end over GNS loopback (S2, #68): raw
// clients against a hosting NetSession. Snapshots reach authenticated peers
// only, the player character goes to the first authenticated peer (never to
// the first input), silent peers are dropped at the auth deadline and
// flooding peers are dropped by the rate limit.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
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
#include "engine/net/replication.hpp"
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
    entt::entity                  crate = entt::null; // static, replicated
    std::optional<net::NetSession> session;

    [[nodiscard]] static std::optional<Host> start(net::HostConfig& config) {
        config.server.port = quat::test::next_loopback_port();
        std::optional<Host> host(std::in_place);
        // A replicated prop (named + Transform) and the player character.
        host->crate = host->registry.create();
        host->registry.emplace<s::Name>(host->crate, "crate");
        host->registry.emplace<s::Transform>(host->crate);
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
    // The first snapshot packet received: its delivery class and net ids.
    struct Snapshot {
        bool                       reliable = false;
        std::vector<std::uint32_t> ids;
    };
    std::optional<Snapshot> first_snapshot;

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

    // Net ids of a snapshot packet (layout in replication.hpp).
    [[nodiscard]] static Snapshot parse_snapshot(const net::NetEvent& ev) {
        Snapshot s;
        s.reliable = ev.reliable;
        for (std::size_t at = net::snapshot_header_bytes;
             at + net::snapshot_entry_bytes <= ev.payload.size();
             at += net::snapshot_entry_bytes) {
            std::uint32_t id = 0;
            std::memcpy(&id, ev.payload.data() + at, sizeof(id));
            s.ids.push_back(id);
        }
        return s;
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
                if (!first_snapshot) first_snapshot = parse_snapshot(ev);
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
    run(*host, {&*intruder}, [&] { return intruder->was_connected; }, 0.0F);
    ASSERT_TRUE(intruder->was_connected);
    const auto input = net::encode_input({1U, glm::vec3(1.0F, 0.0F, 0.0F), k_frame});
    ASSERT_TRUE(intruder->client->send(input, /*reliable=*/true));
    run(*host, {&*intruder}, [&] { return intruder->end_reason.has_value(); }, 0.0F);
    EXPECT_EQ(intruder->end_reason, reason(net::CloseReason::auth_failed));
    EXPECT_EQ(host->session->owner(), net::invalid_client);
    EXPECT_FALSE(host->registry.all_of<s::NetDriven>(host->player));

    // A wrong token gets nothing either.
    auto guesser = Peer::connect(config.server.port, "guess");
    ASSERT_TRUE(guesser.has_value());
    run(*host, {&*guesser}, [&] { return guesser->end_reason.has_value(); }, 0.0F);
    EXPECT_EQ(guesser->end_reason, reason(net::CloseReason::auth_failed));
    EXPECT_EQ(host->session->owner(), net::invalid_client);

    // The first authenticated peer gets the character.
    auto player = Peer::connect(config.server.port, "letmein");
    ASSERT_TRUE(player.has_value());
    run(*host, {&*player}, [&] { return host->session->owner() != net::invalid_client; },
        0.0F);
    const net::ClientId owner = host->session->owner();
    ASSERT_NE(owner, net::invalid_client);
    EXPECT_TRUE(host->session->access().authenticated(owner));
    host->frame();
    EXPECT_TRUE(host->registry.all_of<s::NetDriven>(host->player))
        << "the remote owner drives the character";
}

TEST(NetSessionHost, SilentPeersAreDroppedAtTheAuthDeadline) {
    net::HostConfig config;
    config.access.auth_timeout = 0.5F; // 10 host frames of k_frame
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());

    auto silent = Peer::connect(config.server.port, std::nullopt);
    auto player = Peer::connect(config.server.port, "");
    ASSERT_TRUE(silent && player);
    // Host time stands still (dt = 0) while both connect and the player
    // authenticates, so wall-clock delays cannot eat into the deadline.
    run(*host, {&*silent, &*player},
        [&] {
            return silent->was_connected && host->session->access().peer_count() == 2U &&
                   host->session->access().recipients().size() == 1U;
        },
        0.0F);
    ASSERT_EQ(host->session->access().peer_count(), 2U);
    ASSERT_EQ(host->session->access().recipients().size(), 1U);

    // The deadline runs in host time only: dropped after 0.5 s of frames.
    int frames = 0;
    while (frames < 100 && host->session->access().peer_count() == 2U) {
        host->frame();
        ++frames;
    }
    EXPECT_GE(frames, 10);
    EXPECT_LE(frames, 11);
    run(*host, {&*silent, &*player}, [&] { return silent->end_reason.has_value(); }, 0.0F);

    EXPECT_EQ(silent->end_reason, reason(net::CloseReason::auth_timeout));
    EXPECT_FALSE(player->end_reason.has_value()) << "the authenticated peer stays";
    EXPECT_EQ(host->session->access().peer_count(), 1U);
}

// A peer that authenticates mid-session has no delta baseline: its first
// snapshot must be a reliable keyframe carrying static entities too.
TEST(NetSessionHost, AJoinersFirstSnapshotIsAKeyframe) {
    net::HostConfig config;
    config.access.auth_timeout = 60.0F;
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());
    const auto* rep = host->registry.try_get<s::NetReplicated>(host->crate);
    ASSERT_TRUE(rep != nullptr && rep->net_id != 0U);
    const std::uint32_t crate_id = rep->net_id;

    // Peer A authenticates (host time frozen), gets the first keyframe ...
    auto first = Peer::connect(config.server.port, "");
    ASSERT_TRUE(first.has_value());
    run(*host, {&*first},
        [&] { return host->session->access().recipients().size() == 1U; }, 0.0F);
    ASSERT_EQ(host->session->access().recipients().size(), 1U);
    host->frame(); // one snapshot interval: the first keyframe
    run(*host, {&*first}, [&] { return first->first_snapshot.has_value(); }, 0.0F);
    ASSERT_TRUE(first->first_snapshot.has_value());
    EXPECT_TRUE(first->first_snapshot->reliable);
    // ... and the session moves on: a few delta intervals, which carry
    // nothing (the crate never moves), far from the next periodic keyframe.
    for (int i = 0; i < 3; ++i) host->frame();

    // Peer B authenticates mid-cycle; exactly one snapshot interval passes.
    auto second = Peer::connect(config.server.port, "");
    ASSERT_TRUE(second.has_value());
    run(*host, {&*first, &*second},
        [&] { return host->session->access().recipients().size() == 2U; }, 0.0F);
    ASSERT_EQ(host->session->access().recipients().size(), 2U);
    host->frame();
    run(*host, {&*first, &*second}, [&] { return second->first_snapshot.has_value(); }, 0.0F);

    ASSERT_TRUE(second->first_snapshot.has_value())
        << "no snapshot after the joiner authenticated (a delta of a static scene is empty)";
    EXPECT_TRUE(second->first_snapshot->reliable) << "keyframes ride the reliable channel";
    const std::vector<std::uint32_t>& ids = second->first_snapshot->ids;
    EXPECT_NE(std::find(ids.begin(), ids.end(), crate_id), ids.end())
        << "the static crate is missing from the joiner's first snapshot";
}

TEST(NetSessionHost, FloodingPeersAreDropped) {
    net::HostConfig config;
    config.access.max_messages_per_second = 50; // burst: 100 messages
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());

    auto flooder = Peer::connect(config.server.port, "");
    ASSERT_TRUE(flooder.has_value());
    run(*host, {&*flooder},
        [&] { return host->session->access().recipients().size() == 1U; }, 0.0F);
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

// The real client side: a joined NetSession (not a raw peer) presents its
// token in a hello before any input, with drive_character called from the
// first frame as the game does.
struct Joiner {
    entt::registry                 registry;
    entt::entity                   player = entt::null;
    std::optional<net::NetSession> session;

    [[nodiscard]] static std::optional<Joiner> start(std::uint16_t port, std::string token) {
        std::optional<Joiner> joiner(std::in_place);
        joiner->player = joiner->registry.create();
        joiner->registry.emplace<s::Transform>(joiner->player);
        joiner->registry.emplace<s::CharacterController>(joiner->player);
        net::JoinConfig config;
        config.host = "127.0.0.1";
        config.port = port;
        config.token = std::move(token);
        auto session = net::NetSession::join(joiner->registry, config);
        if (!session) {
            ADD_FAILURE() << session.error().message;
            return std::nullopt;
        }
        joiner->session.emplace(std::move(*session));
        return joiner;
    }

    // One game frame: input first, then the network update.
    void frame() {
        session->drive_character(registry, nullptr, player, glm::vec3(1.0F, 0.0F, 0.0F),
                                 k_frame);
        session->update(registry, nullptr, player, k_frame);
    }
};

template <typename Done>
void run_joined(Host& host, Joiner& joiner, Done done) {
    for (int i = 0; i < 2000 && !done(); ++i) {
        joiner.frame();
        host.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

TEST(NetSessionHost, JoinedSessionAuthenticatesAndOwnsTheCharacter) {
    net::HostConfig config;
    config.access.join_token = "letmein";
    config.access.auth_timeout = 60.0F;
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());
    auto joiner = Joiner::start(config.server.port, "letmein");
    ASSERT_TRUE(joiner.has_value());

    run_joined(*host, *joiner, [&] { return host->session->owner() != net::invalid_client; });
    const net::ClientId owner = host->session->owner();
    ASSERT_NE(owner, net::invalid_client) << "the joined session never got the character";
    EXPECT_TRUE(host->session->access().authenticated(owner));

    // Inputs keep flowing for a while without the host dropping anyone.
    int frames = 0;
    run_joined(*host, *joiner, [&] { return ++frames > 30; });
    EXPECT_TRUE(joiner->session->in_session());
    EXPECT_EQ(joiner->session->end_reason(), 0)
        << net::close_reason_name(joiner->session->end_reason());
    EXPECT_EQ(host->session->owner(), owner);
    EXPECT_EQ(host->session->access().peer_count(), 1U);
    EXPECT_TRUE(host->registry.all_of<s::NetDriven>(host->player));
}

TEST(NetSessionHost, JoinedSessionWithTheWrongTokenIsRefused) {
    net::HostConfig config;
    config.access.join_token = "letmein";
    config.access.auth_timeout = 60.0F;
    auto host = Host::start(config);
    ASSERT_TRUE(host.has_value());
    auto joiner = Joiner::start(config.server.port, "guess");
    ASSERT_TRUE(joiner.has_value());

    run_joined(*host, *joiner, [&] { return joiner->session->end_reason() != 0; });
    EXPECT_EQ(joiner->session->end_reason(), reason(net::CloseReason::auth_failed));
    EXPECT_FALSE(joiner->session->in_session());
    EXPECT_EQ(host->session->owner(), net::invalid_client);
    EXPECT_EQ(host->session->access().peer_count(), 0U);
}

} // namespace
