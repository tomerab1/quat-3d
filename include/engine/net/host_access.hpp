#pragma once

// HostAccess: who may use a hosted session (S2, #68).
//
// Every connection the transport accepts starts unauthenticated. Its first
// message must be a hello (protocol version + join token); until then the
// peer receives nothing and owns nothing, and it is dropped if the hello does
// not arrive within AccessConfig::auth_timeout seconds of host time. A wrong
// token, a version mismatch or any other first message drops it at once.
//
// Ownership of the player character goes to the first peer to authenticate
// (never to whoever sends the first input), or, when an owner token is
// configured, only to a peer whose hello presents it. When the owner leaves,
// the longest-authenticated remaining peer allowed to own takes over.
//
// Every peer, authenticated or not, has a per-second message and byte budget
// (token buckets holding rate_burst_seconds' worth); a peer that exceeds either is
// dropped. All payloads are untrusted: hellos are bounds-checked byte for byte.
//
// Transport-free: NetSession feeds it the transport's events and acts on its
// decisions (disconnects, snapshot recipients, ownership), so the policy is
// unit-testable without sockets.
//
// Wire format of the hello (kind 4), little-endian:
//   [u8 kind = 4] [u8 protocol_version] [u16 token length] [token bytes]

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/error.hpp"
#include "engine/net/transport.hpp"

namespace engine::net {

inline constexpr std::uint8_t hello_kind = 4;
inline constexpr std::uint8_t protocol_version = 1;
inline constexpr std::size_t  hello_header_bytes = 1 + 1 + 2; // kind, version, token length
inline constexpr std::size_t  max_token_bytes = 256;
// A peer may burst this many seconds of its rate budget (host hitches deliver
// several frames of input at once).
inline constexpr float rate_burst_seconds = 2.0F;

struct Hello {
    std::uint8_t version = 0;
    std::string  token;
};

// Fails only for a token longer than max_token_bytes.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, core::Error>
encode_hello(std::string_view token);
// nullopt unless `payload` is exactly one well-formed hello (any version).
[[nodiscard]] std::optional<Hello> decode_hello(std::span<const std::uint8_t> payload);

struct AccessConfig {
    // Token every hello must carry; empty: any hello authenticates.
    std::string join_token;
    // When set, only a peer presenting this token may own the player
    // character (it also authenticates the peer); others join without
    // control. Empty: the first authenticated peer owns it.
    std::string owner_token;
    // Seconds of host time a connected peer has to send its hello.
    float auth_timeout = 5.0F;
    // Per-peer inbound budgets. Clients send one input per frame, so the
    // message rate bounds a client's usable frame rate.
    std::uint32_t max_messages_per_second = 1000;
    std::uint32_t max_bytes_per_second = 64U * 1024U;
};

// Tokens within max_token_bytes, a positive finite timeout, non-zero rates.
[[nodiscard]] std::expected<void, core::Error> validate(const AccessConfig& config);

class HostAccess {
public:
    HostAccess() = default;
    // `config` must pass validate().
    explicit HostAccess(AccessConfig config);

    enum class Verdict : std::uint8_t {
        deliver,       // an authenticated peer's message: handle it
        authenticated, // the peer's valid hello (consumed)
        ignore,        // unknown peer or a repeated hello (consumed)
        drop,          // disconnect the peer with `reason`; already forgotten here
    };
    struct Decision {
        Verdict     verdict = Verdict::ignore;
        CloseReason reason = CloseReason::none;
    };

    // Transport events. on_disconnected is for disconnects the transport
    // reports; peers this class drops (drop verdicts, tick) are already gone.
    void on_connected(ClientId client);
    void on_disconnected(ClientId client);
    [[nodiscard]] Decision on_message(ClientId client, std::span<const std::uint8_t> payload);

    // Advance host time by `dt` seconds: refills the rate budgets and returns
    // the peers whose auth deadline passed (forgotten here; the caller
    // disconnects them with CloseReason::auth_timeout). Call once per frame,
    // before the frame's events.
    [[nodiscard]] std::vector<ClientId> tick(float dt);

    [[nodiscard]] bool known(ClientId client) const { return peers_.contains(client); }
    [[nodiscard]] bool authenticated(ClientId client) const;
    // Authenticated peers in the order they authenticated: the only peers
    // that receive snapshots.
    [[nodiscard]] std::span<const ClientId> recipients() const { return authenticated_; }
    // The peer owning the player character (invalid_client: none).
    [[nodiscard]] ClientId owner() const { return owner_; }
    [[nodiscard]] std::size_t peer_count() const { return peers_.size(); }
    [[nodiscard]] const AccessConfig& config() const { return config_; }

private:
    struct Peer {
        float age = 0.0F; // seconds since connected (unauthenticated peers)
        float message_budget = 0.0F;
        float byte_budget = 0.0F;
        bool  authenticated = false;
        bool  may_own = false;
    };

    void forget(ClientId client);
    void elect_owner();

    AccessConfig                           config_;
    std::unordered_map<ClientId, Peer>     peers_;
    std::vector<ClientId>                  authenticated_;
    ClientId                               owner_ = invalid_client;
};

} // namespace engine::net
