#pragma once

// Network transport (Phase 14, Slice 14.1) — GameNetworkingSockets wrapper.
//
// Client/server over GNS's encrypted, connection-oriented UDP. Both ends are
// poll-driven: call poll() once per frame to pump callbacks and drain received
// messages/events — no background threads touch engine state. Messages are
// opaque byte spans on two delivery classes: reliable (ordered, resent) and
// unreliable (latest-wins state snapshots). GNS itself is an implementation
// detail (PIMPL); the library is initialised on first use and torn down with
// the last endpoint.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::net {

using ClientId = std::uint32_t;
inline constexpr ClientId invalid_client = 0;

struct NetEvent {
    enum class Type : std::uint8_t { connected, disconnected, message };
    Type                      type = Type::message;
    ClientId                  client = invalid_client; // server side: who
    std::vector<std::uint8_t> payload;                 // message events only
};

class NetServer {
public:
    [[nodiscard]] static std::expected<NetServer, core::Error> listen(std::uint16_t port);

    NetServer();
    NetServer(NetServer&&) noexcept;
    NetServer& operator=(NetServer&&) noexcept;
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;
    ~NetServer();

    [[nodiscard]] bool valid() const;

    // Pump callbacks and drain everything that arrived since the last call.
    [[nodiscard]] std::vector<NetEvent> poll();

    void send(ClientId client, std::span<const std::uint8_t> bytes, bool reliable);
    void broadcast(std::span<const std::uint8_t> bytes, bool reliable);
    void disconnect(ClientId client);

    struct Impl; // PIMPL; public name so the .cpp's callback router can refer to it

private:
    std::unique_ptr<Impl> impl_;
};

class NetClient {
public:
    // Begins connecting; the connected event arrives via poll().
    [[nodiscard]] static std::expected<NetClient, core::Error> connect(const std::string& host,
                                                                       std::uint16_t port);

    NetClient();
    NetClient(NetClient&&) noexcept;
    NetClient& operator=(NetClient&&) noexcept;
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;
    ~NetClient();

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool connected() const;

    [[nodiscard]] std::vector<NetEvent> poll();
    void send(std::span<const std::uint8_t> bytes, bool reliable);

    struct Impl; // PIMPL; public name so the .cpp's callback router can refer to it

private:
    std::unique_ptr<Impl> impl_;
};

// Loopback self-test: server on 127.0.0.1, client connects, both exchange
// reliable and unreliable messages, clean disconnect events on teardown.
[[nodiscard]] std::expected<void, core::Error> run_transport_self_test();

} // namespace engine::net
