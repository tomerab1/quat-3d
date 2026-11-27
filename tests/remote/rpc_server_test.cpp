// Remote-control JSON-RPC server over loopback TCP: authentication, sync and
// deferred calls, notifications, protocol errors, and the oversize-line guard.

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/remote/rpc_server.hpp"
#include "remote/socket.hpp"
#include "support/expect_ok.hpp"

namespace {

using namespace engine::remote;

constexpr const char* k_token = "s3cret";

[[nodiscard]] std::string text(const Json& j) {
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// The reply's error code, 0 when it is not an error reply. Reads through
// find(): const operator[] on a missing key is undefined in nlohmann.
[[nodiscard]] int error_code_of(const Json& reply) {
    if (!reply.is_object()) return 0;
    const auto err = reply.find("error");
    if (err == reply.end() || !err->is_object()) return 0;
    const auto code = err->find("code");
    return code != err->end() && code->is_number_integer() ? code->get<int>() : 0;
}

// Minimal line client over the engine's socket helpers: pumps the server
// between reads so a single thread can drive both ends.
struct TestClient {
    detail::Socket sock;
    std::string    inbuf;

    [[nodiscard]] bool send_line(const std::string& line) {
        const std::string data = line + "\n";
        std::size_t off = 0;
        for (int spins = 0; off < data.size() && spins < 1000; ++spins) {
            const detail::IoResult r =
                detail::send_some(sock, data.data() + off, data.size() - off);
            if (r.status == detail::IoStatus::ok) {
                off += r.bytes;
            } else if (r.status != detail::IoStatus::would_block) {
                return false;
            }
        }
        return off == data.size();
    }

    // Next line from the server, pumping `server` meanwhile. Empty on timeout
    // or disconnect (`closed` is set on disconnect).
    [[nodiscard]] std::string read_line(RpcServer& server, bool* closed = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const std::size_t nl = inbuf.find('\n'); nl != std::string::npos) {
                std::string line = inbuf.substr(0, nl);
                inbuf.erase(0, nl + 1);
                return line;
            }
            server.poll();
            char buf[4096];
            const detail::IoResult r = detail::recv_some(sock, buf, sizeof(buf));
            if (r.status == detail::IoStatus::ok) {
                inbuf.append(buf, r.bytes);
            } else if (r.status == detail::IoStatus::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                if (closed != nullptr) *closed = true;
                return {};
            }
        }
        return {};
    }

    // The next reply parsed (discarded on parse failure, null on timeout).
    [[nodiscard]] Json read_reply(RpcServer& server) {
        const std::string line = read_line(server);
        if (line.empty()) return nullptr;
        return Json::parse(line, nullptr, false);
    }

    [[nodiscard]] Json request(RpcServer& server, const Json& msg) {
        if (!send_line(text(msg))) return nullptr;
        return read_reply(server);
    }

    // True when the server closes the connection before sending another line.
    [[nodiscard]] bool closed_by(RpcServer& server) {
        bool closed = false;
        (void)read_line(server, &closed);
        return closed;
    }
};

[[nodiscard]] Json call_msg(Json id, const std::string& method, Json params = nullptr) {
    Json msg = {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", method}};
    if (!params.is_null()) msg["params"] = std::move(params);
    return msg;
}

// Server on an ephemeral port with a token, a 4 KiB line limit, and three
// test methods: test.add (sync), test.notify (counts calls), test.later
// (deferred; parks its responder).
class RemoteRpcServer : public ::testing::Test {
protected:
    std::optional<RpcServer> server;
    int                      notified = 0;
    std::vector<Responder>   parked;

    void SetUp() override {
        RpcServerConfig config;
        config.port = 0;
        config.token = k_token;
        config.max_message_bytes = 4096;
        auto listened = RpcServer::listen(config);
        ASSERT_OK(listened);
        server.emplace(std::move(*listened));
        ASSERT_TRUE(server->listening());
        ASSERT_NE(server->port(), 0) << "no port bound";

        server->add_method("test.add", "a + b", [](const Json& p) -> RpcResult {
            if (!p.contains("a") || !p.contains("b") || !p["a"].is_number() ||
                !p["b"].is_number()) {
                return rpc_fail(rpc_code::invalid_params, "need numbers a and b");
            }
            return Json(p["a"].get<double>() + p["b"].get<double>());
        });
        server->add_method("test.notify", "count", [this](const Json&) -> RpcResult {
            ++notified;
            return Json::object();
        });
        server->add_deferred_method(
            "test.later", "replies when released",
            [this](const Json&, Responder r) { parked.push_back(std::move(r)); });
    }

    [[nodiscard]] TestClient connect() {
        auto sock = detail::connect_loopback(server->port());
        EXPECT_OK(sock);
        TestClient c;
        if (sock) c.sock = std::move(*sock);
        return c;
    }

    // A connected client that has completed session.hello.
    [[nodiscard]] TestClient session() {
        TestClient c = connect();
        Json reply = c.request(*server, call_msg("h", "session.hello", {{"token", k_token}}));
        EXPECT_TRUE(reply.is_object() && reply.contains("result") && reply["id"] == "h")
            << "hello failed: " << text(reply);
        return c;
    }

    // Polls until the deferred handler has parked `n` responders.
    void wait_parked(std::size_t n) {
        for (int i = 0; i < 20 && parked.size() < n; ++i) {
            server->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

TEST_F(RemoteRpcServer, InProcessCallRunsSyncMethods) {
    const RpcResult r = server->call("test.add", Json{{"a", 2}, {"b", 3}});
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(*r == Json(5.0)) << text(*r);
}

TEST_F(RemoteRpcServer, InProcessCallRejectsDeferredMethods) {
    const RpcResult r = server->call("test.later", Json::object());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, rpc_code::unavailable);
}

TEST_F(RemoteRpcServer, CallWithoutHelloIsRejectedAndDisconnected) {
    TestClient c = connect();
    Json reply = c.request(*server, call_msg(1, "test.add", {{"a", 1}, {"b", 1}}));
    EXPECT_EQ(error_code_of(reply), rpc_code::unauthorized) << text(reply);
    EXPECT_TRUE(c.closed_by(*server)) << "unauthenticated connection was not closed";
}

TEST_F(RemoteRpcServer, WrongTokenIsRejected) {
    TestClient c = connect();
    Json reply = c.request(*server, call_msg(1, "session.hello", {{"token", "s3creT"}}));
    EXPECT_EQ(error_code_of(reply), rpc_code::unauthorized) << text(reply);
}

TEST_F(RemoteRpcServer, AuthenticatedSyncCallReplies) {
    TestClient c = session();
    Json reply = c.request(*server, call_msg(7, "test.add", {{"a", 20}, {"b", 22}}));
    EXPECT_TRUE(reply.contains("result") && reply["result"] == Json(42.0)) << text(reply);
    EXPECT_TRUE(reply["id"] == 7) << text(reply);
}

TEST_F(RemoteRpcServer, ProtocolErrorsCarryTheirCodes) {
    TestClient c = session();
    Json reply = c.request(*server, call_msg(8, "test.add", {{"a", "x"}}));
    EXPECT_EQ(error_code_of(reply), rpc_code::invalid_params) << text(reply);

    reply = c.request(*server, call_msg(9, "nope"));
    EXPECT_EQ(error_code_of(reply), rpc_code::method_not_found) << text(reply);

    ASSERT_TRUE(c.send_line("{not json"));
    reply = c.read_reply(*server);
    EXPECT_EQ(error_code_of(reply), rpc_code::parse_error) << text(reply);
}

// A notification (no id) runs but gets no reply: the next request's reply
// must be the next line on the wire.
TEST_F(RemoteRpcServer, NotificationRunsWithoutAReply) {
    TestClient c = session();
    ASSERT_TRUE(c.send_line(R"({"jsonrpc":"2.0","method":"test.notify"})"));
    Json reply = c.request(*server, call_msg(10, "rpc.methods"));
    EXPECT_EQ(notified, 1);
    EXPECT_TRUE(reply["id"] == 10 && reply["result"].is_array())
        << "notification produced a reply: " << text(reply);
}

TEST_F(RemoteRpcServer, DeferredMethodRepliesWhenReleased) {
    TestClient c = session();
    ASSERT_TRUE(c.send_line(R"({"jsonrpc":"2.0","id":11,"method":"test.later"})"));
    wait_parked(1);
    ASSERT_EQ(parked.size(), 1U) << "deferred handler not invoked";
    EXPECT_TRUE(c.inbuf.empty()) << "deferred method replied early: " << c.inbuf;

    parked.front().reply(Json("done"));
    parked.clear();
    Json reply = c.read_reply(*server);
    EXPECT_TRUE(reply["id"] == 11 && reply["result"] == "done") << text(reply);
}

TEST_F(RemoteRpcServer, DroppedResponderStillAnswers) {
    TestClient c = session();
    ASSERT_TRUE(c.send_line(R"({"jsonrpc":"2.0","id":12,"method":"test.later"})"));
    wait_parked(1);
    ASSERT_EQ(parked.size(), 1U) << "deferred handler not invoked";
    parked.clear(); // destroyed without replying
    Json reply = c.read_reply(*server);
    EXPECT_TRUE(reply["id"] == 12) << text(reply);
    EXPECT_EQ(error_code_of(reply), rpc_code::internal_error) << text(reply);
}

TEST_F(RemoteRpcServer, OversizedLineIsRejectedAndTheClientReaped) {
    TestClient c = session();
    ASSERT_TRUE(c.send_line(std::string(8000, 'x')));
    Json reply = c.read_reply(*server);
    EXPECT_EQ(error_code_of(reply), rpc_code::invalid_request) << text(reply);
    EXPECT_TRUE(c.closed_by(*server)) << "oversize connection not closed";
    for (int i = 0; i < 10; ++i) server->poll();
    EXPECT_EQ(server->client_count(), 0U) << "dead clients not reaped";
}

} // namespace
