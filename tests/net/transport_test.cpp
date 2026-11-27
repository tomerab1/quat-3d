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

} // namespace
