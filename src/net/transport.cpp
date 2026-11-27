#include "engine/net/transport.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingsockets.h>

namespace engine::net {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// GNS global init, refcounted across endpoints (init once, kill with the last).
int g_gns_refs = 0;

[[nodiscard]] std::expected<void, core::Error> gns_acquire() {
    if (g_gns_refs == 0) {
        SteamDatagramErrMsg err;
        if (!GameNetworkingSockets_Init(nullptr, err)) {
            return fail(std::string("net: GNS init failed: ") + err);
        }
    }
    ++g_gns_refs;
    return {};
}

void gns_release() {
    if (--g_gns_refs == 0) {
        GameNetworkingSockets_Kill();
    }
}

constexpr int send_flags(bool reliable) {
    return reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
}

static_assert(max_message_bytes ==
                  static_cast<std::size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend),
              "transport.hpp's max_message_bytes must match GNS");

constexpr bool in_app_range(CloseReason r) {
    const auto v = static_cast<std::int32_t>(r);
    return v >= k_ESteamNetConnectionEnd_App_Min && v <= k_ESteamNetConnectionEnd_App_Max;
}
static_assert(in_app_range(CloseReason::server_full) && in_app_range(CloseReason::auth_failed) &&
                  in_app_range(CloseReason::auth_timeout) &&
                  in_app_range(CloseReason::rate_limited) && in_app_range(CloseReason::kicked) &&
                  in_app_range(CloseReason::shutdown),
              "close reasons must be in GNS's application range");

void close_with(ISteamNetworkingSockets* sockets, HSteamNetConnection conn, CloseReason reason) {
    const std::string name = close_reason_name(static_cast<std::int32_t>(reason));
    sockets->CloseConnection(conn, static_cast<int>(reason), name.c_str(), false);
}

// One message to one connection; false if it is oversized or GNS refused it
// (send queue full, connection closed).
[[nodiscard]] bool send_one(ISteamNetworkingSockets* sockets, HSteamNetConnection conn,
                            std::span<const std::uint8_t> bytes, bool reliable) {
    if (bytes.size() > max_message_bytes) return false;
    return sockets->SendMessageToConnection(conn, bytes.data(),
                                            static_cast<std::uint32_t>(bytes.size()),
                                            send_flags(reliable), nullptr) == k_EResultOK;
}

// GNS standalone exposes ONE global interface: RunCallbacks() from any
// endpoint dispatches every pending callback, so events must be routed to
// their owning endpoint by listen socket / connection handle. The engine is
// single-threaded, so plain vectors suffice.
void global_on_status(SteamNetConnectionStatusChangedCallback_t* info);

} // namespace

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
struct NetServer::Impl {
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket       listen_socket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup       poll_group = k_HSteamNetPollGroup_Invalid;
    std::uint32_t            max_clients = default_max_clients;
    ClientId                 next_id = 1;
    std::unordered_map<HSteamNetConnection, ClientId> clients;
    std::unordered_map<ClientId, HSteamNetConnection> connections;
    std::vector<NetEvent> pending;

    void on_status(SteamNetConnectionStatusChangedCallback_t* info);
    ~Impl();
};

struct NetClient::Impl {
    ISteamNetworkingSockets* sockets = nullptr;
    HSteamNetConnection      connection = k_HSteamNetConnection_Invalid;
    bool                     is_connected = false;
    std::vector<NetEvent>    pending;

    void on_status(SteamNetConnectionStatusChangedCallback_t* info);
    ~Impl();
};

namespace {

std::vector<NetServer::Impl*> g_servers;
std::vector<NetClient::Impl*> g_clients;

void global_on_status(SteamNetConnectionStatusChangedCallback_t* info) {
    for (NetServer::Impl* srv : g_servers) {
        if (info->m_info.m_hListenSocket == srv->listen_socket ||
            srv->clients.contains(info->m_hConn)) {
            srv->on_status(info);
            return;
        }
    }
    for (NetClient::Impl* cli : g_clients) {
        if (info->m_hConn == cli->connection) {
            cli->on_status(info);
            return;
        }
    }
}

template <typename T>
void unregister(std::vector<T*>& v, T* p) {
    v.erase(std::remove(v.begin(), v.end(), p), v.end());
}

} // namespace

void NetServer::Impl::on_status(SteamNetConnectionStatusChangedCallback_t* info) {
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
        // Only the cap is enforced here; authentication is the session's
        // (host_access.hpp). Handshaking connections count, so a flood of
        // half-open connections cannot exceed it either.
        if (clients.size() >= max_clients) {
            close_with(sockets, info->m_hConn, CloseReason::server_full);
            break;
        }
        if (sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
            sockets->CloseConnection(info->m_hConn, 0, "accept failed", false);
            break;
        }
        sockets->SetConnectionPollGroup(info->m_hConn, poll_group);
        // Claim the connection now so later events route here.
        clients.emplace(info->m_hConn, invalid_client);
        break;
    case k_ESteamNetworkingConnectionState_Connected: {
        const ClientId id = next_id++;
        clients[info->m_hConn] = id;
        connections.emplace(id, info->m_hConn);
        NetEvent ev;
        ev.type = NetEvent::Type::connected;
        ev.client = id;
        pending.push_back(std::move(ev));
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        if (const auto it = clients.find(info->m_hConn); it != clients.end()) {
            if (it->second != invalid_client) {
                NetEvent ev;
                ev.type = NetEvent::Type::disconnected;
                ev.client = it->second;
                pending.push_back(std::move(ev));
                connections.erase(it->second);
            }
            clients.erase(it);
        }
        sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
        break;
    }
    default:
        break;
    }
}

NetServer::Impl::~Impl() {
    unregister(g_servers, this);
    if (sockets != nullptr) {
        for (const auto& [conn, id] : clients) close_with(sockets, conn, CloseReason::shutdown);
        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            sockets->DestroyPollGroup(poll_group);
        }
        if (listen_socket != k_HSteamListenSocket_Invalid) {
            sockets->CloseListenSocket(listen_socket);
        }
        gns_release();
    }
}

NetServer::NetServer() = default;
NetServer::NetServer(NetServer&&) noexcept = default;
NetServer& NetServer::operator=(NetServer&&) noexcept = default;
NetServer::~NetServer() = default;
bool NetServer::valid() const { return impl_ != nullptr; }

std::expected<NetServer, core::Error> NetServer::listen(std::uint16_t port) {
    ServerConfig config;
    config.port = port;
    return listen(config);
}

std::expected<NetServer, core::Error> NetServer::listen(const ServerConfig& config) {
    // Validate before touching GNS.
    const std::optional<std::uint32_t> ip = parse_ipv4(config.bind);
    if (!ip) {
        return fail("net: bad bind address '" + config.bind +
                    "' (expected an IPv4 address such as 127.0.0.1 or 0.0.0.0)");
    }
    if (config.max_clients == 0) return fail("net: max_clients must be at least 1");
    if (auto r = gns_acquire(); !r) return std::unexpected(r.error());

    NetServer out;
    out.impl_ = std::make_unique<Impl>();
    out.impl_->sockets = SteamNetworkingSockets();
    out.impl_->max_clients = config.max_clients;

    // An IPv4 address (0.0.0.0 included) binds an IPv4-only socket on that
    // interface; GNS's dual-stack "all zeros" IPv6 bind is never used.
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.SetIPv4(*ip, config.port);

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&global_on_status));
    out.impl_->listen_socket = out.impl_->sockets->CreateListenSocketIP(addr, 1, &opt);
    if (out.impl_->listen_socket == k_HSteamListenSocket_Invalid) {
        return fail("net: listen socket creation failed on " + config.bind + ":" +
                    std::to_string(config.port) + " (port in use, or no such interface?)");
    }
    out.impl_->poll_group = out.impl_->sockets->CreatePollGroup();
    g_servers.push_back(out.impl_.get());
    return out;
}

std::vector<NetEvent> NetServer::poll() {
    if (!valid()) return {};
    impl_->sockets->RunCallbacks();

    std::vector<NetEvent> events = std::move(impl_->pending);
    impl_->pending.clear();

    SteamNetworkingMessage_t* msgs[32];
    int n = 0;
    while ((n = impl_->sockets->ReceiveMessagesOnPollGroup(impl_->poll_group, msgs, 32)) > 0) {
        for (int i = 0; i < n; ++i) {
            // Only connections that finished the handshake (and were not
            // dropped since) have a client id; anything else is discarded.
            const auto it = impl_->clients.find(msgs[i]->m_conn);
            if (it == impl_->clients.end() || it->second == invalid_client) {
                msgs[i]->Release();
                continue;
            }
            NetEvent ev;
            ev.type = NetEvent::Type::message;
            ev.client = it->second;
            const auto* data = static_cast<const std::uint8_t*>(msgs[i]->m_pData);
            ev.payload.assign(data, data + msgs[i]->m_cbSize);
            events.push_back(std::move(ev));
            msgs[i]->Release();
        }
    }
    return events;
}

bool NetServer::send(ClientId client, std::span<const std::uint8_t> bytes, bool reliable) {
    if (!valid()) return false;
    const auto it = impl_->connections.find(client);
    return it != impl_->connections.end() &&
           send_one(impl_->sockets, it->second, bytes, reliable);
}

bool NetServer::broadcast(std::span<const std::uint8_t> bytes, bool reliable) {
    if (!valid()) return false;
    bool all_sent = true;
    for (const auto& [id, conn] : impl_->connections) {
        all_sent = send_one(impl_->sockets, conn, bytes, reliable) && all_sent;
    }
    return all_sent;
}

bool NetServer::send_to(std::span<const ClientId> clients, std::span<const std::uint8_t> bytes,
                        bool reliable) {
    bool all_sent = true;
    for (const ClientId client : clients) all_sent = send(client, bytes, reliable) && all_sent;
    return all_sent;
}

void NetServer::disconnect(ClientId client, CloseReason reason) {
    if (!valid()) return;
    if (const auto it = impl_->connections.find(client); it != impl_->connections.end()) {
        close_with(impl_->sockets, it->second, reason);
        impl_->clients.erase(it->second);
        impl_->connections.erase(it);
    }
}

std::size_t NetServer::connection_count() const { return valid() ? impl_->clients.size() : 0; }

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
void NetClient::Impl::on_status(SteamNetConnectionStatusChangedCallback_t* info) {
    if (info->m_hConn != connection) return;
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connected: {
        is_connected = true;
        NetEvent ev;
        ev.type = NetEvent::Type::connected;
        pending.push_back(std::move(ev));
        break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        if (is_connected ||
            info->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
            NetEvent ev;
            ev.type = NetEvent::Type::disconnected;
            ev.end_reason = info->m_info.m_eEndReason;
            pending.push_back(std::move(ev));
        }
        is_connected = false;
        sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
        connection = k_HSteamNetConnection_Invalid;
        break;
    }
    default:
        break;
    }
}

NetClient::Impl::~Impl() {
    unregister(g_clients, this);
    if (sockets != nullptr) {
        if (connection != k_HSteamNetConnection_Invalid) {
            sockets->CloseConnection(connection, 0, "client shutdown", false);
        }
        gns_release();
    }
}

NetClient::NetClient() = default;
NetClient::NetClient(NetClient&&) noexcept = default;
NetClient& NetClient::operator=(NetClient&&) noexcept = default;
NetClient::~NetClient() = default;
bool NetClient::valid() const { return impl_ != nullptr; }
bool NetClient::connected() const { return valid() && impl_->is_connected; }

std::expected<NetClient, core::Error> NetClient::connect(const std::string& host,
                                                         std::uint16_t port) {
    if (auto r = gns_acquire(); !r) return std::unexpected(r.error());

    NetClient out;
    out.impl_ = std::make_unique<Impl>();
    out.impl_->sockets = SteamNetworkingSockets();

    const std::optional<std::uint32_t> ip = parse_ipv4(host);
    if (!ip) return fail("net: bad IPv4 address '" + host + "'");
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.SetIPv4(*ip, port);

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&global_on_status));
    out.impl_->connection = out.impl_->sockets->ConnectByIPAddress(addr, 1, &opt);
    if (out.impl_->connection == k_HSteamNetConnection_Invalid) {
        return fail("net: connect failed to start");
    }
    g_clients.push_back(out.impl_.get());
    return out;
}

std::vector<NetEvent> NetClient::poll() {
    if (!valid()) return {};
    impl_->sockets->RunCallbacks();

    std::vector<NetEvent> events = std::move(impl_->pending);
    impl_->pending.clear();
    if (impl_->connection == k_HSteamNetConnection_Invalid) return events;

    SteamNetworkingMessage_t* msgs[32];
    int n = 0;
    while ((n = impl_->sockets->ReceiveMessagesOnConnection(impl_->connection, msgs, 32)) > 0) {
        for (int i = 0; i < n; ++i) {
            NetEvent ev;
            ev.type = NetEvent::Type::message;
            const auto* data = static_cast<const std::uint8_t*>(msgs[i]->m_pData);
            ev.payload.assign(data, data + msgs[i]->m_cbSize);
            events.push_back(std::move(ev));
            msgs[i]->Release();
        }
    }
    return events;
}

bool NetClient::send(std::span<const std::uint8_t> bytes, bool reliable) {
    return connected() && send_one(impl_->sockets, impl_->connection, bytes, reliable);
}

} // namespace engine::net
