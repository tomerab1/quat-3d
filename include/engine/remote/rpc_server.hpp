#pragma once

// RpcServer — engine remote-control endpoint (Phase 15, Slice 15.1).
//
// A JSON-RPC 2.0 server over loopback TCP, one JSON message per line
// (newline-delimited — compact JSON never contains a raw newline). It is how
// out-of-process tools drive the engine; the quat-mcp bridge (tools/quat-mcp)
// exposes it to language models over the Model Context Protocol.
//
// Threading: poll-driven like engine_net. The frame loop calls poll() once per
// frame on the main thread; it accepts connections, reads whatever arrived,
// runs the handlers inline (so they may touch the ECS freely) and flushes
// replies. No background thread touches engine state.
//
// Security: the listener binds 127.0.0.1 only. When a token is configured,
// every connection must open with `session.hello {"token": ...}` before any
// other method is accepted; anything else is answered "unauthorized" and the
// connection is closed.
//
// Methods are either synchronous (return a result or an RpcError) or deferred:
// a deferred handler receives a one-shot Responder it may complete on a later
// frame (e.g. a screenshot that resolves when the GPU readback lands). A
// Responder destroyed without replying answers "request dropped", so clients
// never hang; replies to clients that disconnected are discarded.

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"

namespace engine::remote {

using Json = nlohmann::json;

// JSON-RPC error codes: the spec's reserved range plus engine-specific ones.
namespace rpc_code {
inline constexpr int parse_error      = -32700;
inline constexpr int invalid_request  = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params   = -32602;
inline constexpr int internal_error   = -32603;
inline constexpr int unauthorized     = -32001; // missing/wrong session token
inline constexpr int unavailable      = -32002; // not supported in this build / state
inline constexpr int not_found        = -32003; // a referenced entity/asset does not exist
inline constexpr int failed           = -32004; // the operation itself failed
} // namespace rpc_code

struct RpcError {
    int         code = rpc_code::internal_error;
    std::string message;
    Json        data; // optional structured detail (null = omitted)
};

[[nodiscard]] inline std::unexpected<RpcError> rpc_fail(int code, std::string message) {
    return std::unexpected(RpcError{code, std::move(message), nullptr});
}

using RpcResult = std::expected<Json, RpcError>;

class RpcServer;

// One-shot reply channel of a deferred request. Move-only; replying twice is
// ignored. Safe to outlive (or be moved across) the server.
class Responder {
public:
    Responder() = default;
    Responder(Responder&& other) noexcept;
    Responder& operator=(Responder&& other) noexcept;
    Responder(const Responder&) = delete;
    Responder& operator=(const Responder&) = delete;
    ~Responder();

    void reply(RpcResult result);
    [[nodiscard]] bool pending() const { return state_ != nullptr && !done_; }

private:
    friend class RpcServer;
    struct Target;
    Responder(std::shared_ptr<Target> state, std::uint32_t client, Json id);

    std::shared_ptr<Target> state_;
    std::uint32_t           client_ = 0;
    Json                    id_;
    bool                    done_ = false;
};

struct RpcServerConfig {
    std::uint16_t port = 7878;     // 0 = pick an ephemeral port (tests)
    std::string   token;           // empty = no authentication
    std::size_t   max_clients = 4;
    std::size_t   max_message_bytes = 16U << 20; // inbound line limit
    std::size_t   max_pending_output = 256U << 20; // per-client backlog before drop
    int           max_requests_per_poll = 512;    // bounds per-frame work
};

struct MethodInfo {
    std::string name;
    std::string summary;
    bool        deferred = false;
};

class RpcServer {
public:
    using SyncHandler = std::function<RpcResult(const Json& params)>;
    using DeferredHandler = std::function<void(const Json& params, Responder responder)>;

    // Binds 127.0.0.1:<port> (non-blocking). The returned server already has
    // the built-in methods (session.hello, rpc.methods) registered.
    [[nodiscard]] static std::expected<RpcServer, core::Error> listen(RpcServerConfig config);

    // A server with no socket: methods can be registered and invoked through
    // call() (tests, in-process batching).
    RpcServer();
    RpcServer(RpcServer&&) noexcept;
    RpcServer& operator=(RpcServer&&) noexcept;
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    ~RpcServer();

    [[nodiscard]] bool listening() const;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::size_t client_count() const;

    void add_method(std::string name, std::string summary, SyncHandler handler);
    void add_deferred_method(std::string name, std::string summary, DeferredHandler handler);
    [[nodiscard]] std::vector<MethodInfo> methods() const;

    // Invoke a synchronous method in-process. Deferred methods are rejected
    // (`unavailable`) — they cannot complete inside a single call.
    [[nodiscard]] RpcResult call(const std::string& method, const Json& params);

    // Accept, read, dispatch, flush. Call once per frame on the main thread.
    void poll();

    struct Impl; // public name so Responder's target can refer to it

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::remote
