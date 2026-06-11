#include "engine/net/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
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
        // Accept everyone; auth belongs to the replication layer.
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
        for (const auto& [conn, id] : clients) {
            sockets->CloseConnection(conn, 0, "server shutdown", false);
        }
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
    if (auto r = gns_acquire(); !r) return std::unexpected(r.error());

    NetServer out;
    out.impl_ = std::make_unique<Impl>();
    out.impl_->sockets = SteamNetworkingSockets();

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&global_on_status));
    out.impl_->listen_socket = out.impl_->sockets->CreateListenSocketIP(addr, 1, &opt);
    if (out.impl_->listen_socket == k_HSteamListenSocket_Invalid) {
        return fail("net: listen socket creation failed (port in use?)");
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
            NetEvent ev;
            ev.type = NetEvent::Type::message;
            if (const auto it = impl_->clients.find(msgs[i]->m_conn);
                it != impl_->clients.end()) {
                ev.client = it->second;
            }
            const auto* data = static_cast<const std::uint8_t*>(msgs[i]->m_pData);
            ev.payload.assign(data, data + msgs[i]->m_cbSize);
            events.push_back(std::move(ev));
            msgs[i]->Release();
        }
    }
    return events;
}

void NetServer::send(ClientId client, std::span<const std::uint8_t> bytes, bool reliable) {
    if (!valid()) return;
    if (const auto it = impl_->connections.find(client); it != impl_->connections.end()) {
        impl_->sockets->SendMessageToConnection(it->second, bytes.data(),
                                                static_cast<std::uint32_t>(bytes.size()),
                                                send_flags(reliable), nullptr);
    }
}

void NetServer::broadcast(std::span<const std::uint8_t> bytes, bool reliable) {
    if (!valid()) return;
    for (const auto& [id, conn] : impl_->connections) {
        impl_->sockets->SendMessageToConnection(conn, bytes.data(),
                                                static_cast<std::uint32_t>(bytes.size()),
                                                send_flags(reliable), nullptr);
    }
}

void NetServer::disconnect(ClientId client) {
    if (!valid()) return;
    if (const auto it = impl_->connections.find(client); it != impl_->connections.end()) {
        impl_->sockets->CloseConnection(it->second, 0, "kicked", false);
        impl_->clients.erase(it->second);
        impl_->connections.erase(it);
    }
}

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

    // Parse the IPv4 dotted quad ourselves: SteamNetworkingIPAddr::ParseString
    // is not exported from the Windows DLL build (SetIPv4 is header-inline).
    const std::string ip = host == "localhost" ? "127.0.0.1" : host;
    std::uint32_t octets[4] = {0, 0, 0, 0};
    int parsed = 0;
    std::size_t start = 0;
    for (int i = 0; i < 4 && start <= ip.size(); ++i) {
        const std::size_t dot = ip.find('.', start);
        const std::string part = ip.substr(start, dot == std::string::npos ? std::string::npos
                                                                           : dot - start);
        if (part.empty() || part.size() > 3) break;
        octets[i] = static_cast<std::uint32_t>(std::atoi(part.c_str()));
        if (octets[i] > 255) break;
        ++parsed;
        if (dot == std::string::npos) {
            start = ip.size() + 1;
            break;
        }
        start = dot + 1;
    }
    if (parsed != 4) {
        return fail("net: bad IPv4 address '" + host + "'");
    }
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.SetIPv4((octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3], port);

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

void NetClient::send(std::span<const std::uint8_t> bytes, bool reliable) {
    if (!connected()) return;
    impl_->sockets->SendMessageToConnection(impl_->connection, bytes.data(),
                                            static_cast<std::uint32_t>(bytes.size()),
                                            send_flags(reliable), nullptr);
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error> run_transport_self_test() {
    auto server = NetServer::listen(42777);
    if (!server) return std::unexpected(server.error());
    auto client = NetClient::connect("127.0.0.1", 42777);
    if (!client) return std::unexpected(client.error());

    // Pump both ends until the handshake completes (encrypted: a few RTTs).
    bool server_saw_connect = false;
    ClientId id = invalid_client;
    for (int i = 0; i < 500 && !(server_saw_connect && client->connected()); ++i) {
        for (const NetEvent& ev : server->poll()) {
            if (ev.type == NetEvent::Type::connected) {
                server_saw_connect = true;
                id = ev.client;
            }
        }
        (void)client->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!server_saw_connect || !client->connected()) {
        return fail("transport self-test: loopback handshake never completed");
    }

    // Client -> server reliable; server -> client unreliable echo.
    const std::uint8_t ping[] = {'p', 'i', 'n', 'g'};
    client->send(ping, /*reliable=*/true);
    bool got_ping = false;
    for (int i = 0; i < 500 && !got_ping; ++i) {
        for (const NetEvent& ev : server->poll()) {
            if (ev.type == NetEvent::Type::message && ev.payload.size() == 4 &&
                std::memcmp(ev.payload.data(), "ping", 4) == 0 && ev.client == id) {
                got_ping = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!got_ping) return fail("transport self-test: reliable message lost");

    const std::uint8_t pong[] = {'p', 'o', 'n', 'g'};
    server->send(id, pong, /*reliable=*/false);
    server->broadcast(pong, /*reliable=*/false);
    bool got_pong = false;
    for (int i = 0; i < 500 && !got_pong; ++i) {
        (void)server->poll();
        for (const NetEvent& ev : client->poll()) {
            if (ev.type == NetEvent::Type::message && ev.payload.size() == 4 &&
                std::memcmp(ev.payload.data(), "pong", 4) == 0) {
                got_pong = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!got_pong) return fail("transport self-test: unreliable message lost on loopback");
    return {};
}

} // namespace engine::net
