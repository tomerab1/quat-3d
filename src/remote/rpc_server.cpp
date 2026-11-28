// RpcServer — loopback JSON-RPC 2.0 endpoint (Phase 15, Slice 15.1).
// See include/engine/remote/rpc_server.hpp for the protocol contract.

#include "engine/remote/rpc_server.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "socket.hpp"

namespace engine::remote {

namespace {

constexpr int k_protocol_version = 1;

[[nodiscard]] std::string dump_compact(const Json& j) {
    // Replace invalid UTF-8 instead of throwing (which aborts without
    // exceptions): entity names from glTF files are not guaranteed valid.
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

[[nodiscard]] Json error_object(const RpcError& e) {
    Json err = {{"code", e.code}, {"message", e.message}};
    if (!e.data.is_null()) err["data"] = e.data;
    return err;
}

[[nodiscard]] Json make_response(const Json& id, const RpcResult& result) {
    Json msg = {{"jsonrpc", "2.0"}, {"id", id}};
    if (result) {
        msg["result"] = *result;
    } else {
        msg["error"] = error_object(result.error());
    }
    return msg;
}

// Constant-time comparison: the token check must not leak a prefix match
// through timing.
[[nodiscard]] bool token_equal(std::string_view a, std::string_view b) {
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(i < a.size() ? a[i] : 0);
        const auto cb = static_cast<unsigned char>(i < b.size() ? b[i] : 0);
        diff |= static_cast<unsigned>(ca ^ cb);
    }
    return diff == 0;
}

// RFC 9110 token characters (HTTP methods and header names).
[[nodiscard]] bool is_tchar(char ch) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0) return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos;
}

// True for a line of an HTTP message: a request line (`POST / HTTP/1.1`), a
// status line (`HTTP/1.1 200 OK`) or a header (`Host: 127.0.0.1`). A browser
// can POST to the loopback port without a CORS preflight, so the request line
// must end the connection before any later line — the body — is read
// (issue #56). JSON-RPC requests start with '{' and never match.
[[nodiscard]] bool looks_like_http(std::string_view line) {
    if (line.starts_with("HTTP/")) return true;
    std::size_t i = 0;
    while (i < line.size() && is_tchar(line[i])) ++i;
    if (i == 0 || i == line.size()) return false;
    if (line[i] == ':') return true; // header field
    if (line[i] != ' ') return false;
    // Request line: method SP target SP HTTP-version.
    const std::size_t target_end = line.find(' ', i + 1);
    return target_end != std::string_view::npos && target_end > i + 1 &&
           line.substr(target_end + 1).starts_with("HTTP/");
}

struct Method {
    MethodInfo                   info;
    RpcServer::SyncHandler       sync;
    RpcServer::DeferredHandler   deferred;
};

struct Client {
    std::uint32_t id = 0;
    detail::Socket sock;
    std::string    inbuf;
    std::string    outbuf;
    std::size_t    out_offset = 0;
    bool           authed = false;
    bool           closing = false; // flush pending output, then drop
    bool           dead = false;    // drop immediately
};

} // namespace

// Shared between the server and its outstanding Responders: lets a Responder
// reach the client's outbox while the server lives, and become a no-op once
// it is gone.
struct Responder::Target {
    RpcServer::Impl* impl = nullptr;
};

struct RpcServer::Impl {
    RpcServerConfig                          config;
    detail::Socket                           listener;
    std::uint16_t                            port = 0;
    std::vector<Client>                      clients;
    std::uint32_t                            next_client = 1;
    std::unordered_map<std::string, Method>  methods;
    std::shared_ptr<Responder::Target>       target = std::make_shared<Responder::Target>();

    Impl() { target->impl = this; }
    ~Impl() { target->impl = nullptr; }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Client* find_client(std::uint32_t id) {
        for (Client& c : clients) {
            if (c.id == id && !c.dead) return &c;
        }
        return nullptr;
    }

    void queue(Client& c, const Json& msg) {
        if (c.dead) return;
        c.outbuf += dump_compact(msg);
        c.outbuf += '\n';
        if (c.outbuf.size() - c.out_offset > config.max_pending_output) {
            c.dead = true; // a client that stops reading must not grow us unbounded
        }
    }

    void deliver(std::uint32_t client, const Json& id, const RpcResult& result) {
        if (Client* c = find_client(client)) queue(*c, make_response(id, result));
    }

    void register_builtins();
    void handle_line(Client& c, std::string_view line);
    void read_client(Client& c);
    void flush_client(Client& c);
};

// ---------------------------------------------------------------------------
// Responder
// ---------------------------------------------------------------------------

Responder::Responder(std::shared_ptr<Target> state, std::uint32_t client, Json id)
    : state_(std::move(state)), client_(client), id_(std::move(id)) {}

Responder::Responder(Responder&& other) noexcept
    : state_(std::move(other.state_)),
      client_(other.client_),
      id_(std::move(other.id_)),
      done_(std::exchange(other.done_, true)) {}

Responder& Responder::operator=(Responder&& other) noexcept {
    if (this != &other) {
        if (pending()) reply(rpc_fail(rpc_code::internal_error, "request dropped"));
        state_ = std::move(other.state_);
        client_ = other.client_;
        id_ = std::move(other.id_);
        done_ = std::exchange(other.done_, true);
    }
    return *this;
}

Responder::~Responder() {
    if (pending()) reply(rpc_fail(rpc_code::internal_error, "request dropped"));
}

void Responder::reply(RpcResult result) {
    if (!pending()) return;
    done_ = true;
    if (state_->impl != nullptr) state_->impl->deliver(client_, id_, result);
}

// ---------------------------------------------------------------------------
// RpcServer
// ---------------------------------------------------------------------------

RpcServer::RpcServer() : impl_(std::make_unique<Impl>()) { impl_->register_builtins(); }
RpcServer::RpcServer(RpcServer&&) noexcept = default;
RpcServer& RpcServer::operator=(RpcServer&&) noexcept = default;
RpcServer::~RpcServer() = default;

std::expected<RpcServer, core::Error> RpcServer::listen(RpcServerConfig config) {
    if (config.token.empty()) {
        return std::unexpected(core::Error{
            "the remote-control endpoint needs a session token (RpcServerConfig::token)"});
    }
    RpcServer server;
    auto sock = detail::listen_loopback(config.port);
    if (!sock) return std::unexpected(sock.error());
    server.impl_->port = detail::local_port(*sock);
    server.impl_->listener = std::move(*sock);
    server.impl_->config = std::move(config);
    return server;
}

bool RpcServer::listening() const { return impl_ && impl_->listener.valid(); }
std::uint16_t RpcServer::port() const { return impl_ ? impl_->port : 0; }
std::size_t RpcServer::client_count() const { return impl_ ? impl_->clients.size() : 0; }

void RpcServer::add_method(std::string name, std::string summary, SyncHandler handler) {
    Method m;
    m.info = {name, std::move(summary), false, nullptr, nullptr};
    m.sync = std::move(handler);
    impl_->methods.insert_or_assign(std::move(name), std::move(m));
}

void RpcServer::add_deferred_method(std::string name, std::string summary,
                                    DeferredHandler handler) {
    Method m;
    m.info = {name, std::move(summary), true, nullptr, nullptr};
    m.deferred = std::move(handler);
    impl_->methods.insert_or_assign(std::move(name), std::move(m));
}

bool RpcServer::set_method_schemas(const std::string& name, Json params_schema, Json result_schema) {
    const auto it = impl_->methods.find(name);
    if (it == impl_->methods.end()) return false;
    it->second.info.params_schema = std::move(params_schema);
    it->second.info.result_schema = std::move(result_schema);
    return true;
}

std::vector<MethodInfo> RpcServer::methods() const {
    std::vector<MethodInfo> out;
    out.reserve(impl_->methods.size());
    for (const auto& [name, m] : impl_->methods) out.push_back(m.info);
    std::sort(out.begin(), out.end(),
              [](const MethodInfo& a, const MethodInfo& b) { return a.name < b.name; });
    return out;
}

RpcResult RpcServer::call(const std::string& method, const Json& params) {
    const auto it = impl_->methods.find(method);
    if (it == impl_->methods.end()) {
        return rpc_fail(rpc_code::method_not_found, "unknown method '" + method + "'");
    }
    if (!it->second.sync) {
        return rpc_fail(rpc_code::unavailable,
                        "'" + method + "' completes asynchronously and cannot be nested");
    }
    // Copy the handler: it may register/unregister methods while running.
    const SyncHandler handler = it->second.sync;
    return handler(params);
}

void RpcServer::Impl::register_builtins() {
    // session.hello is answered inline by handle_line (it needs the client);
    // registering it here lists it in rpc.methods.
    Method hello;
    hello.info = {"session.hello",
                  "Authenticate ({token}) and get the protocol version. Required as the first "
                  "call on every connection.",
                  false,
                  nullptr,
                  nullptr};
    hello.sync = [](const Json&) -> RpcResult { return Json::object(); };
    methods.insert_or_assign("session.hello", std::move(hello));

    Method list;
    list.info = {"rpc.methods", "List every method with a one-line summary.", false, nullptr,
                 nullptr};
    list.sync = [this](const Json&) -> RpcResult {
        std::vector<const Method*> sorted;
        for (const auto& [name, m] : methods) sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [](const Method* a, const Method* b) {
            return a->info.name < b->info.name;
        });
        Json out = Json::array();
        for (const Method* m : sorted) {
            Json entry = {{"name", m->info.name},
                          {"summary", m->info.summary},
                          {"deferred", m->info.deferred}};
            if (!m->info.params_schema.is_null()) entry["params_schema"] = m->info.params_schema;
            if (!m->info.result_schema.is_null()) entry["result_schema"] = m->info.result_schema;
            out.push_back(std::move(entry));
        }
        return out;
    };
    methods.insert_or_assign("rpc.methods", std::move(list));
}

void RpcServer::Impl::handle_line(Client& c, std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) return;

    // Protocol hygiene (issue #56): HTTP is dropped without a reply, and the
    // first line that is not a JSON-RPC 2.0 request object gets an error and
    // ends the connection — a cross-protocol client never reaches a later line.
    if (looks_like_http(line)) {
        c.dead = true;
        return;
    }
    const Json msg = Json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded()) {
        queue(c, make_response(nullptr, rpc_fail(rpc_code::parse_error, "invalid JSON")));
        c.closing = true;
        return;
    }
    if (!msg.is_object()) {
        queue(c, make_response(nullptr, rpc_fail(rpc_code::invalid_request,
                                                 "expected a JSON-RPC request object "
                                                 "(batch arrays are not supported)")));
        c.closing = true;
        return;
    }
    const bool is_notification = !msg.contains("id");
    Json id = nullptr;
    if (!is_notification) {
        const Json& raw_id = msg["id"];
        if (raw_id.is_string() || raw_id.is_number_integer() || raw_id.is_null()) id = raw_id;
    }
    const auto reply = [&](const RpcResult& r) {
        if (!is_notification) queue(c, make_response(id, r));
    };

    const auto jsonrpc = msg.find("jsonrpc");
    const auto method_it = msg.find("method");
    if (jsonrpc == msg.end() || !jsonrpc->is_string() || *jsonrpc != "2.0" ||
        method_it == msg.end() || !method_it->is_string()) {
        queue(c, make_response(id, rpc_fail(rpc_code::invalid_request,
                                            "a request needs \"jsonrpc\": \"2.0\" and a "
                                            "string \"method\"")));
        c.closing = true;
        return;
    }
    const std::string& method = method_it->get_ref<const std::string&>();
    if (!c.authed && method != "session.hello") {
        queue(c, make_response(id, rpc_fail(rpc_code::unauthorized,
                                            "the first call must be session.hello with the "
                                            "session token")));
        c.closing = true;
        return;
    }
    Json params = Json::object();
    if (const auto p = msg.find("params"); p != msg.end() && !p->is_null()) {
        if (!p->is_object()) {
            reply(rpc_fail(rpc_code::invalid_params, "params must be an object"));
            if (!c.authed) c.closing = true;
            return;
        }
        params = *p;
    }

    if (method == "session.hello") {
        const auto tok = params.find("token");
        if (tok == params.end() || !tok->is_string() ||
            !token_equal(tok->get_ref<const std::string&>(), config.token)) {
            queue(c, make_response(id, rpc_fail(rpc_code::unauthorized,
                                                tok == params.end() ? "missing session token"
                                                                    : "invalid session token")));
            c.closing = true;
            return;
        }
        c.authed = true;
        reply(Json{{"protocol", k_protocol_version},
                   {"server", "quat-3d"},
                   {"methods", methods.size()}});
        return;
    }
    const auto it = methods.find(method);
    if (it == methods.end()) {
        reply(rpc_fail(rpc_code::method_not_found,
                       "unknown method '" + method + "' (rpc.methods lists them)"));
        return;
    }
    if (it->second.deferred) {
        // Notifications get a responder bound to no client: replies vanish.
        const DeferredHandler handler = it->second.deferred;
        handler(params, Responder(target, is_notification ? 0U : c.id, id));
        return;
    }
    const SyncHandler handler = it->second.sync;
    reply(handler(params));
}

void RpcServer::Impl::read_client(Client& c) {
    char buf[64 * 1024];
    for (;;) {
        const detail::IoResult r = detail::recv_some(c.sock, buf, sizeof(buf));
        if (r.status == detail::IoStatus::would_block) break;
        if (r.status != detail::IoStatus::ok) {
            c.dead = true; // closed by peer or error
            return;
        }
        c.inbuf.append(buf, r.bytes);
        if (c.inbuf.size() > config.max_message_bytes + sizeof(buf)) break; // checked below
    }
}

void RpcServer::Impl::flush_client(Client& c) {
    while (!c.dead && c.out_offset < c.outbuf.size()) {
        const detail::IoResult r = detail::send_some(c.sock, c.outbuf.data() + c.out_offset,
                                                     c.outbuf.size() - c.out_offset);
        if (r.status == detail::IoStatus::would_block) break;
        if (r.status != detail::IoStatus::ok) {
            c.dead = true;
            return;
        }
        c.out_offset += r.bytes;
    }
    if (c.out_offset == c.outbuf.size()) {
        c.outbuf.clear();
        c.out_offset = 0;
        if (c.closing) c.dead = true;
    } else if (c.out_offset > (1U << 20)) {
        c.outbuf.erase(0, c.out_offset); // compact occasionally
        c.out_offset = 0;
    }
}

void RpcServer::poll() {
    Impl& s = *impl_;
    if (!s.listener.valid()) return;

    // Accept everything pending; refuse beyond the client cap.
    for (;;) {
        auto accepted = detail::accept_client(s.listener);
        if (!accepted) break;
        if (s.clients.size() >= s.config.max_clients) {
            Client refused;
            refused.sock = std::move(*accepted);
            refused.closing = true;
            s.queue(refused, make_response(nullptr, rpc_fail(rpc_code::unavailable,
                                                             "too many remote clients")));
            s.flush_client(refused);
            continue;
        }
        Client c;
        c.id = s.next_client++;
        if (s.next_client == 0) s.next_client = 1; // 0 means "no client"
        c.sock = std::move(*accepted);
        s.clients.push_back(std::move(c));
    }

    int budget = s.config.max_requests_per_poll;
    // Index loop: handlers may complete deferred replies to other clients,
    // but never add or remove clients, so indices stay valid.
    for (std::size_t i = 0; i < s.clients.size(); ++i) {
        if (s.clients[i].dead || s.clients[i].closing) continue;
        s.read_client(s.clients[i]);
        std::size_t start = 0;
        for (;;) {
            Client& c = s.clients[i];
            if (c.dead || c.closing) break;
            const std::size_t nl = c.inbuf.find('\n', start);
            const std::size_t line_len =
                (nl == std::string::npos ? c.inbuf.size() : nl) - start;
            if (nl == std::string::npos || line_len > s.config.max_message_bytes) {
                if (line_len > s.config.max_message_bytes) {
                    s.queue(c, make_response(nullptr,
                                             rpc_fail(rpc_code::invalid_request,
                                                      "message exceeds " +
                                                          std::to_string(s.config.max_message_bytes) +
                                                          " bytes")));
                    c.closing = true;
                }
                break;
            }
            if (budget <= 0) break; // resume next frame
            --budget;
            const std::string line = c.inbuf.substr(start, nl - start);
            start = nl + 1;
            s.handle_line(c, line);
        }
        Client& c = s.clients[i];
        if (start > 0) c.inbuf.erase(0, std::min(start, c.inbuf.size()));
    }

    for (Client& c : s.clients) s.flush_client(c);
    std::erase_if(s.clients, [](const Client& c) { return c.dead; });
}

} // namespace engine::remote
