// Transport helpers that need no GameNetworkingSockets: address parsing and
// close-reason names (declared in transport.hpp).

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "engine/net/transport.hpp"

namespace engine::net {

// Parsed here rather than with SteamNetworkingIPAddr::ParseString, which the
// Windows GNS DLL does not export (SetIPv4 is header-inline). Strict: exactly
// four decimal parts of 1-3 digits, no signs, spaces or trailing text.
std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
    if (text == "localhost") text = loopback_address;
    std::uint32_t address = 0;
    std::size_t   at = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (at >= text.size() || text[at] != '.') return std::nullopt;
            ++at;
        }
        const std::size_t begin = at;
        while (at < text.size() && at - begin < 4 && text[at] >= '0' && text[at] <= '9') ++at;
        if (at == begin || at - begin > 3) return std::nullopt;
        unsigned octet = 0;
        const auto [end, ec] = std::from_chars(text.data() + begin, text.data() + at, octet);
        if (ec != std::errc{} || end != text.data() + at || octet > 255U) return std::nullopt;
        address = (address << 8U) | octet;
    }
    if (at != text.size()) return std::nullopt;
    return address;
}

std::string close_reason_name(std::int32_t end_reason) {
    switch (static_cast<CloseReason>(end_reason)) {
    case CloseReason::none:
        return "unknown";
    case CloseReason::server_full:
        return "server full";
    case CloseReason::auth_failed:
        return "authentication failed";
    case CloseReason::auth_timeout:
        return "authentication timed out";
    case CloseReason::rate_limited:
        return "rate limited";
    case CloseReason::kicked:
        return "kicked";
    case CloseReason::shutdown:
        return "shut down";
    }
    return "transport end code " + std::to_string(end_reason);
}

} // namespace engine::net
