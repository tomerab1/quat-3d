#pragma once

// A GameNetworkingSockets server and client connected over 127.0.0.1, for the
// transport and replication tests. Every pair listens on its own port so
// tests never see a previous test's lingering connection.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <thread>
#include <utility>

#include "engine/core/error.hpp"
#include "engine/net/transport.hpp"

namespace quat::test {

// A port no earlier test in this process used (lingering connections never
// leak between tests).
[[nodiscard]] inline std::uint16_t next_loopback_port() {
    static std::uint16_t next_port = 42777;
    return next_port++;
}

struct LoopbackPair {
    std::optional<engine::net::NetServer> server;
    std::optional<engine::net::NetClient> client;
    engine::net::ClientId                 id = engine::net::invalid_client; // server side

    // Starts listening on a fresh port and begins connecting the client.
    [[nodiscard]] std::expected<void, engine::core::Error> open() {
        const std::uint16_t port = next_loopback_port();
        auto s = engine::net::NetServer::listen(port);
        if (!s) return std::unexpected(s.error());
        server.emplace(std::move(*s));
        auto c = engine::net::NetClient::connect("127.0.0.1", port);
        if (!c) return std::unexpected(c.error());
        client.emplace(std::move(*c));
        return {};
    }

    // Pumps both ends until the (encrypted, a few round trips) handshake
    // completes on both sides; false if it never does.
    [[nodiscard]] bool handshake() {
        bool server_saw_connect = false;
        for (int i = 0; i < 500 && !(server_saw_connect && client->connected()); ++i) {
            for (const engine::net::NetEvent& ev : server->poll()) {
                if (ev.type == engine::net::NetEvent::Type::connected) {
                    server_saw_connect = true;
                    id = ev.client;
                }
            }
            (void)client->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return server_saw_connect && client->connected();
    }
};

} // namespace quat::test
