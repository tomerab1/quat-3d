// GameNetworkingSockets transport over loopback: the encrypted handshake,
// reliable client -> server and unreliable server -> client delivery, and
// up-front refusal of oversized messages.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "engine/net/transport.hpp"
#include "net/loopback.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::net::NetEvent;

[[nodiscard]] bool is_message(const NetEvent& ev, const char (&text)[5]) {
    return ev.type == NetEvent::Type::message && ev.payload.size() == 4 &&
           std::memcmp(ev.payload.data(), text, 4) == 0;
}

class NetTransport : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_OK(pair.open());
        ASSERT_TRUE(pair.handshake()) << "loopback handshake never completed";
    }

    quat::test::LoopbackPair pair;
};

TEST_F(NetTransport, HandshakeConnectsBothEnds) {
    EXPECT_TRUE(pair.client->connected());
    EXPECT_NE(pair.id, engine::net::invalid_client);
}

TEST_F(NetTransport, ReliableClientMessageReachesTheServer) {
    const std::uint8_t ping[] = {'p', 'i', 'n', 'g'};
    ASSERT_TRUE(pair.client->send(ping, /*reliable=*/true)) << "reliable send refused";

    bool got_ping = false;
    for (int i = 0; i < 500 && !got_ping; ++i) {
        for (const NetEvent& ev : pair.server->poll()) {
            got_ping = got_ping || (is_message(ev, "ping") && ev.client == pair.id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(got_ping) << "reliable message lost";
}

TEST_F(NetTransport, UnreliableSendAndBroadcastReachTheClient) {
    const std::uint8_t pong[] = {'p', 'o', 'n', 'g'};
    ASSERT_TRUE(pair.server->send(pair.id, pong, /*reliable=*/false));
    ASSERT_TRUE(pair.server->broadcast(pong, /*reliable=*/false));

    bool got_pong = false;
    for (int i = 0; i < 500 && !got_pong; ++i) {
        (void)pair.server->poll();
        for (const NetEvent& ev : pair.client->poll()) got_pong = got_pong || is_message(ev, "pong");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(got_pong) << "unreliable message lost on loopback";
}

// Oversized messages are refused up front, not dropped silently.
TEST_F(NetTransport, OversizedMessagesAreRefused) {
    const std::vector<std::uint8_t> oversized(engine::net::max_message_bytes + 1, 0);
    EXPECT_FALSE(pair.server->send(pair.id, oversized, /*reliable=*/true));
    EXPECT_FALSE(pair.client->send(oversized, /*reliable=*/true));
}

// A disconnect tells the peer why.
TEST_F(NetTransport, DisconnectCarriesTheReason) {
    pair.server->disconnect(pair.id, engine::net::CloseReason::rate_limited);
    EXPECT_EQ(pair.server->connection_count(), 0U);
    std::int32_t reason = -1;
    for (int i = 0; i < 500 && reason == -1; ++i) {
        for (const NetEvent& ev : pair.client->poll()) {
            if (ev.type == NetEvent::Type::disconnected) reason = ev.end_reason;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(reason, static_cast<std::int32_t>(engine::net::CloseReason::rate_limited));
}

// --- host hardening (#68) ---------------------------------------------------

TEST(NetTransportConfig, DefaultsToLoopbackAndACap) {
    const engine::net::ServerConfig config;
    EXPECT_EQ(config.bind, "127.0.0.1");
    EXPECT_EQ(config.max_clients, engine::net::default_max_clients);
}

TEST(NetTransportConfig, InvalidConfigsAreRefused) {
    engine::net::ServerConfig config;
    config.port = quat::test::next_loopback_port();
    config.bind = "everywhere";
    EXPECT_FALSE(engine::net::NetServer::listen(config).has_value());
    config.bind = "127.0.0.1";
    config.max_clients = 0;
    EXPECT_FALSE(engine::net::NetServer::listen(config).has_value());
}

// Binding every interface is possible when asked for; loopback still reaches it.
TEST(NetTransportConfig, AllInterfacesOptIn) {
    engine::net::ServerConfig config;
    config.port = quat::test::next_loopback_port();
    config.bind = "0.0.0.0";
    auto server = engine::net::NetServer::listen(config);
    ASSERT_OK(server);
    auto client = engine::net::NetClient::connect("127.0.0.1", config.port);
    ASSERT_OK(client);
    for (int i = 0; i < 500 && !client->connected(); ++i) {
        (void)server->poll();
        (void)client->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(client->connected());
}

// The (max_clients + 1)th connection is refused with server_full; the first
// one is unaffected, and its slot is free again once it leaves.
TEST(NetTransportConfig, ConnectionsBeyondTheCapAreRefused) {
    engine::net::ServerConfig config;
    config.port = quat::test::next_loopback_port();
    config.max_clients = 1;
    auto server = engine::net::NetServer::listen(config);
    ASSERT_OK(server);

    auto first = engine::net::NetClient::connect("127.0.0.1", config.port);
    ASSERT_OK(first);
    for (int i = 0; i < 500 && !first->connected(); ++i) {
        (void)server->poll();
        (void)first->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(first->connected());

    auto second = engine::net::NetClient::connect("127.0.0.1", config.port);
    ASSERT_OK(second);
    std::int32_t refused = -1;
    for (int i = 0; i < 1000 && refused == -1; ++i) {
        (void)server->poll();
        (void)first->poll();
        for (const NetEvent& ev : second->poll()) {
            if (ev.type == NetEvent::Type::disconnected) refused = ev.end_reason;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(refused, static_cast<std::int32_t>(engine::net::CloseReason::server_full));
    EXPECT_FALSE(second->connected());
    EXPECT_TRUE(first->connected());
    EXPECT_EQ(server->connection_count(), 1U);

    // The slot frees up when the first client leaves.
    first.value() = engine::net::NetClient();
    for (int i = 0; i < 1000 && server->connection_count() != 0; ++i) {
        (void)server->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(server->connection_count(), 0U);
    auto third = engine::net::NetClient::connect("127.0.0.1", config.port);
    ASSERT_OK(third);
    for (int i = 0; i < 500 && !third->connected(); ++i) {
        (void)server->poll();
        (void)third->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(third->connected());
}

} // namespace
