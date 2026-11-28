#include "engine/net/session_config.hpp"

#include <charconv>
#include <string_view>
#include <utility>

namespace engine::net {

namespace {

[[nodiscard]] std::unexpected<core::Error> bad(std::string_view var, std::string_view value,
                                               std::string_view expected) {
    return std::unexpected(core::make_error(std::string(var) + "='" + std::string(value) +
                                            "': expected " + std::string(expected)));
}

// Whole-string unsigned decimal within [lo, hi].
[[nodiscard]] std::optional<std::uint32_t> parse_uint(std::string_view text, std::uint32_t lo,
                                                      std::uint32_t hi) {
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || value < lo ||
        value > hi) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::expected<std::uint16_t, core::Error> parse_port(std::string_view var,
                                                                   std::string_view text) {
    const std::optional<std::uint32_t> port = parse_uint(text, 1, 65535);
    if (!port) return bad(var, text, "a port number 1-65535");
    return static_cast<std::uint16_t>(*port);
}

[[nodiscard]] std::expected<std::string, core::Error> read_token(const EnvLookup& lookup,
                                                                 const char* var) {
    const char* value = lookup(var);
    if (value == nullptr) return std::string();
    const std::string_view token(value);
    if (token.size() > max_token_bytes) {
        return bad(var, "<hidden>",
                   "a token of at most " + std::to_string(max_token_bytes) + " bytes");
    }
    return std::string(token);
}

[[nodiscard]] std::expected<HostConfig, core::Error> host_config(const EnvLookup& lookup,
                                                                 std::string_view port_text) {
    HostConfig config;
    auto port = parse_port("QUAT_HOST", port_text);
    if (!port) return std::unexpected(port.error());
    config.server.port = *port;

    if (const char* bind = lookup("QUAT_NET_BIND"); bind != nullptr) {
        if (!parse_ipv4(bind)) {
            return bad("QUAT_NET_BIND", bind, "an IPv4 address (127.0.0.1, 0.0.0.0, ...)");
        }
        config.server.bind = bind;
    }
    if (const char* cap = lookup("QUAT_NET_MAX_CLIENTS"); cap != nullptr) {
        const std::optional<std::uint32_t> n = parse_uint(cap, 1, max_max_clients);
        if (!n) {
            return bad("QUAT_NET_MAX_CLIENTS", cap,
                       "a count 1-" + std::to_string(max_max_clients));
        }
        config.server.max_clients = *n;
    }
    auto join_token = read_token(lookup, "QUAT_NET_TOKEN");
    if (!join_token) return std::unexpected(join_token.error());
    config.access.join_token = std::move(*join_token);
    auto owner_token = read_token(lookup, "QUAT_NET_OWNER_TOKEN");
    if (!owner_token) return std::unexpected(owner_token.error());
    config.access.owner_token = std::move(*owner_token);
    if (auto ok = validate(config.access); !ok) return std::unexpected(ok.error());
    return config;
}

[[nodiscard]] std::expected<JoinConfig, core::Error> join_config(const EnvLookup& lookup,
                                                                 std::string_view addr) {
    const std::size_t colon = addr.rfind(':');
    if (colon == std::string_view::npos) return bad("QUAT_JOIN", addr, "<ip:port>");
    JoinConfig config;
    config.host = std::string(addr.substr(0, colon));
    if (!parse_ipv4(config.host)) {
        return bad("QUAT_JOIN", addr, "<ip:port> with an IPv4 address or localhost");
    }
    auto port = parse_port("QUAT_JOIN", addr.substr(colon + 1));
    if (!port) return std::unexpected(port.error());
    config.port = *port;
    auto token = read_token(lookup, "QUAT_NET_TOKEN");
    if (!token) return std::unexpected(token.error());
    config.token = std::move(*token);
    return config;
}

} // namespace

std::expected<SessionConfig, core::Error> session_config_from_env(const EnvLookup& lookup) {
    SessionConfig config;
    if (const char* host = lookup("QUAT_HOST"); host != nullptr) {
        auto h = host_config(lookup, host);
        if (!h) return std::unexpected(h.error());
        config.host = std::move(*h);
    } else if (const char* join = lookup("QUAT_JOIN"); join != nullptr) {
        auto j = join_config(lookup, join);
        if (!j) return std::unexpected(j.error());
        config.join = std::move(*j);
    }
    return config;
}

} // namespace engine::net
