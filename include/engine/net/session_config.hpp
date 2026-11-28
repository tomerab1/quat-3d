#pragma once

// Session configuration: what NetSession hosts or joins, read from the
// environment (S2, #68).
//
//   QUAT_HOST=<port>            host the running scene on <port>
//   QUAT_JOIN=<ip:port>         join a host (QUAT_HOST wins when both are set)
//   QUAT_NET_BIND=<ipv4>        host: interface to listen on; default 127.0.0.1
//                               (this machine only). 0.0.0.0 opts in to every
//                               interface (LAN / internet play).
//   QUAT_NET_MAX_CLIENTS=<n>    host: connections held at once (default 8)
//   QUAT_NET_TOKEN=<secret>     host: token every joiner must present;
//                               joiner: the token it presents
//   QUAT_NET_OWNER_TOKEN=<s>    host: only a joiner presenting this token (as
//                               its QUAT_NET_TOKEN) may control the player
//
// Every value is validated; a bad one is an error naming the variable, and
// the session stays offline rather than guessing.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>

#include "engine/core/error.hpp"
#include "engine/net/host_access.hpp"
#include "engine/net/transport.hpp"

namespace engine::net {

inline constexpr std::uint32_t max_max_clients = 1024;

struct HostConfig {
    ServerConfig server; // port, bind address, connection cap
    AccessConfig access; // tokens, auth deadline, per-peer rate limits
};

struct JoinConfig {
    std::string   host; // IPv4 address or "localhost"
    std::uint16_t port = 0;
    std::string   token; // sent in the hello; empty when the host needs none
};

struct SessionConfig {
    std::optional<HostConfig> host;
    std::optional<JoinConfig> join; // only when not hosting
};

// Reads a variable: nullptr when unset (std::getenv's contract).
using EnvLookup = std::function<const char*(const char*)>;

// Neither QUAT_HOST nor QUAT_JOIN set: an empty config (offline).
[[nodiscard]] std::expected<SessionConfig, core::Error>
session_config_from_env(const EnvLookup& lookup);

} // namespace engine::net
