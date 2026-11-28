#include "engine/net/host_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::net {

namespace {

// Compares without an early exit on the first differing byte, so response
// timing does not reveal how much of a guessed token was right. (The length
// is not secret: it is bounded and the loop covers the longer input.)
[[nodiscard]] bool tokens_equal(std::string_view a, std::string_view b) {
    const std::size_t n = std::max(a.size(), b.size());
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0U;
        const auto y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0U;
        diff |= x ^ y;
    }
    return diff == 0U;
}

[[nodiscard]] float burst_capacity(std::uint32_t per_second) {
    return static_cast<float>(per_second) * rate_burst_seconds;
}

} // namespace

std::expected<std::vector<std::uint8_t>, core::Error> encode_hello(std::string_view token) {
    if (token.size() > max_token_bytes) {
        return std::unexpected(core::make_error("net: join token longer than " +
                                                std::to_string(max_token_bytes) + " bytes"));
    }
    std::vector<std::uint8_t> out(hello_header_bytes + token.size());
    out[0] = hello_kind;
    out[1] = protocol_version;
    const auto length = static_cast<std::uint16_t>(token.size());
    std::memcpy(out.data() + 2, &length, 2);
    if (!token.empty()) std::memcpy(out.data() + hello_header_bytes, token.data(), token.size());
    return out;
}

std::optional<Hello> decode_hello(std::span<const std::uint8_t> payload) {
    if (payload.size() < hello_header_bytes || payload[0] != hello_kind) return std::nullopt;
    std::uint16_t length = 0;
    std::memcpy(&length, payload.data() + 2, 2);
    if (length > max_token_bytes || payload.size() != hello_header_bytes + length) {
        return std::nullopt;
    }
    Hello hello;
    hello.version = payload[1];
    hello.token.assign(reinterpret_cast<const char*>(payload.data() + hello_header_bytes), length);
    return hello;
}

std::expected<void, core::Error> validate(const AccessConfig& config) {
    if (config.join_token.size() > max_token_bytes || config.owner_token.size() > max_token_bytes) {
        return std::unexpected(core::make_error("net: tokens are limited to " +
                                                std::to_string(max_token_bytes) + " bytes"));
    }
    if (!std::isfinite(config.auth_timeout) || !(config.auth_timeout > 0.0F)) {
        return std::unexpected(core::make_error("net: auth_timeout must be a positive number"));
    }
    if (config.max_messages_per_second == 0 || config.max_bytes_per_second == 0) {
        return std::unexpected(core::make_error("net: per-peer rate limits must be non-zero"));
    }
    return {};
}

HostAccess::HostAccess(AccessConfig config) : config_(std::move(config)) {}

void HostAccess::on_connected(ClientId client) {
    if (client == invalid_client) return;
    forget(client); // ids are not reused by the transport; never inherit state
    Peer peer;
    peer.message_budget = burst_capacity(config_.max_messages_per_second);
    peer.byte_budget = burst_capacity(config_.max_bytes_per_second);
    peers_.emplace(client, peer);
}

void HostAccess::on_disconnected(ClientId client) { forget(client); }

bool HostAccess::authenticated(ClientId client) const {
    const auto it = peers_.find(client);
    return it != peers_.end() && it->second.authenticated;
}

HostAccess::Decision HostAccess::on_message(ClientId client,
                                            std::span<const std::uint8_t> payload) {
    const auto it = peers_.find(client);
    if (it == peers_.end()) return {Verdict::ignore, CloseReason::none};
    Peer& peer = it->second;

    // Rate first: an unauthenticated flood costs its sender the connection.
    const auto bytes = static_cast<float>(payload.size());
    if (peer.message_budget < 1.0F || peer.byte_budget < bytes) {
        forget(client);
        return {Verdict::drop, CloseReason::rate_limited};
    }
    peer.message_budget -= 1.0F;
    peer.byte_budget -= bytes;

    if (peer.authenticated) {
        // A repeated hello is harmless; anything else is the session's.
        const bool hello = !payload.empty() && payload[0] == hello_kind;
        return {hello ? Verdict::ignore : Verdict::deliver, CloseReason::none};
    }

    // The first message must be a hello with the right version and token.
    const std::optional<Hello> hello = decode_hello(payload);
    const bool owner_token = hello && !config_.owner_token.empty() &&
                             tokens_equal(hello->token, config_.owner_token);
    const bool join_token = hello && (config_.join_token.empty() ||
                                      tokens_equal(hello->token, config_.join_token));
    if (!hello || hello->version != protocol_version || !(owner_token || join_token)) {
        forget(client);
        return {Verdict::drop, CloseReason::auth_failed};
    }
    peer.authenticated = true;
    peer.may_own = config_.owner_token.empty() || owner_token;
    authenticated_.push_back(client);
    elect_owner();
    return {Verdict::authenticated, CloseReason::none};
}

std::vector<ClientId> HostAccess::tick(float dt) {
    if (!std::isfinite(dt) || dt < 0.0F) dt = 0.0F;
    const float message_cap = burst_capacity(config_.max_messages_per_second);
    const float byte_cap = burst_capacity(config_.max_bytes_per_second);
    std::vector<ClientId> late;
    for (auto& [client, peer] : peers_) {
        peer.message_budget = std::min(
            peer.message_budget + dt * static_cast<float>(config_.max_messages_per_second),
            message_cap);
        peer.byte_budget = std::min(
            peer.byte_budget + dt * static_cast<float>(config_.max_bytes_per_second), byte_cap);
        if (peer.authenticated) continue;
        peer.age += dt;
        if (peer.age >= config_.auth_timeout) late.push_back(client);
    }
    std::sort(late.begin(), late.end()); // deterministic order for the caller
    for (const ClientId client : late) forget(client);
    return late;
}

void HostAccess::forget(ClientId client) {
    if (peers_.erase(client) == 0) return;
    authenticated_.erase(std::remove(authenticated_.begin(), authenticated_.end(), client),
                         authenticated_.end());
    if (owner_ == client) {
        owner_ = invalid_client;
        elect_owner();
    }
}

void HostAccess::elect_owner() {
    if (owner_ != invalid_client) return;
    for (const ClientId client : authenticated_) {
        if (const auto it = peers_.find(client); it != peers_.end() && it->second.may_own) {
            owner_ = client;
            return;
        }
    }
}

} // namespace engine::net
