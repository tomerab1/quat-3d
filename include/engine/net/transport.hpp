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
//
// Host hardening (S2, #68): the server binds loopback unless told otherwise
// (ServerConfig::bind) and refuses connections beyond ServerConfig::max_clients
// with CloseReason::server_full. Authentication, ownership and rate limits sit
// one layer up (host_access.hpp); the transport stays policy-free.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::net {

using ClientId = std::uint32_t;
inline constexpr ClientId invalid_client = 0;

// GNS's per-message send limit (k_cbMaxSteamNetworkingSocketsMessageSizeSend);
// larger messages are refused. Unreliable messages above the path MTU
// (~1200 B) are fragmented and lost whole if any fragment drops, so
// latency-sensitive streams stay well under it (see replication.hpp).
inline constexpr std::size_t max_message_bytes = 512U * 1024U;

// Why the engine closed a connection: sent to the peer with the close (GNS's
// application range, 1000-1999) and reported in the peer's disconnected event.
enum class CloseReason : std::int32_t {
    none = 0,
    server_full = 1001,  // ServerConfig::max_clients reached
    auth_failed = 1002,  // first message not a valid hello, or wrong token
    auth_timeout = 1003, // no hello within AccessConfig::auth_timeout
    rate_limited = 1004, // over AccessConfig's per-peer message / byte rate
    kicked = 1005,       // dropped by the host (NetServer::disconnect default)
    shutdown = 1006,     // the other endpoint was destroyed
};

// Readable name of a disconnected event's end_reason: engine reasons by name,
// GNS's own codes (timeouts, ...) as "transport end code <n>".
[[nodiscard]] std::string close_reason_name(std::int32_t end_reason);

struct NetEvent {
    enum class Type : std::uint8_t { connected, disconnected, message };
    Type                      type = Type::message;
    ClientId                  client = invalid_client; // server side: who
    std::vector<std::uint8_t> payload;                 // message events only
    bool                      reliable = false;        // message events: sent reliably
    // Client side, disconnected events: why the connection ended (a
    // CloseReason when the host closed it, else a GNS end code; 0 = unknown).
    std::int32_t end_reason = 0;
};

// Dotted-quad IPv4 ("a.b.c.d", decimal, each 0-255) or "localhost", as a
// host-order address; nullopt for anything else.
[[nodiscard]] std::optional<std::uint32_t> parse_ipv4(std::string_view text);

inline constexpr std::string_view loopback_address = "127.0.0.1";
inline constexpr std::uint32_t    default_max_clients = 8;

struct ServerConfig {
    std::uint16_t port = 0;
    // IPv4 address to listen on. Loopback (local play only) by default;
    // "0.0.0.0" listens on every interface and has to be asked for.
    std::string bind = std::string(loopback_address);
    // Connections held at once, handshaking ones included; further ones are
    // refused with CloseReason::server_full. At least 1.
    std::uint32_t max_clients = default_max_clients;
};

class NetServer {
public:
    // 127.0.0.1:`port` with the default connection cap.
    [[nodiscard]] static std::expected<NetServer, core::Error> listen(std::uint16_t port);
    [[nodiscard]] static std::expected<NetServer, core::Error> listen(const ServerConfig& config);

    NetServer();
    NetServer(NetServer&&) noexcept;
    NetServer& operator=(NetServer&&) noexcept;
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;
    ~NetServer();

    [[nodiscard]] bool valid() const;

    // Pump callbacks and drain everything that arrived since the last call.
    [[nodiscard]] std::vector<NetEvent> poll();

    // Messages up to max_message_bytes. send() is false when the client is
    // unknown or GNS refused the message (oversized, send queue full, closed
    // connection); broadcast() / send_to() are false if any connection
    // refused it.
    [[nodiscard]] bool send(ClientId client, std::span<const std::uint8_t> bytes, bool reliable);
    [[nodiscard]] bool broadcast(std::span<const std::uint8_t> bytes, bool reliable);
    [[nodiscard]] bool send_to(std::span<const ClientId> clients,
                               std::span<const std::uint8_t> bytes, bool reliable);

    // Close the connection and tell the peer why. No disconnected event
    // follows for a client closed this way.
    void disconnect(ClientId client, CloseReason reason = CloseReason::kicked);

    // Connections held now (handshaking + connected), as max_clients counts.
    [[nodiscard]] std::size_t connection_count() const;

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
    // False when not connected or GNS refused the message (see NetServer::send).
    [[nodiscard]] bool send(std::span<const std::uint8_t> bytes, bool reliable);

    struct Impl; // PIMPL; public name so the .cpp's callback router can refer to it

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::net
