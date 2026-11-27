#pragma once

// Shared fixture for the world-command tests: a real Scene with the world
// commands registered on an in-process RpcServer (RpcServer::call), plus
// helpers that unwrap results.

#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/remote/rpc_server.hpp"
#include "engine/remote/world_commands.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace quat::test {

using engine::remote::Json;
using engine::remote::RpcResult;

class WorldCommands : public ::testing::Test {
protected:
    engine::scene::Scene         scene;
    engine::remote::RpcServer    server;
    engine::remote::WorldServices services; // tests fill these before register_commands()

    void register_commands() { engine::remote::register_world_commands(server, scene, services); }
    void SetUp() override {} // derived fixtures call register_commands() when ready

    [[nodiscard]] RpcResult call(const std::string& method, const Json& params = Json::object()) {
        return server.call(method, params);
    }
    // Calls and expects success; returns the result (null on failure).
    Json ok(const std::string& method, const Json& params = Json::object()) {
        RpcResult r = call(method, params);
        EXPECT_TRUE(r.has_value()) << method << " " << params.dump() << " failed: "
                                   << (r ? "" : r.error().message);
        return r ? *r : Json();
    }
    // Calls and expects the given error code; returns the message.
    std::string err(const std::string& method, const Json& params, int code) {
        RpcResult r = call(method, params);
        EXPECT_FALSE(r.has_value()) << method << " " << params.dump() << " unexpectedly succeeded";
        if (r) return {};
        EXPECT_EQ(r.error().code, code) << r.error().message;
        return r.error().message;
    }
    [[nodiscard]] entt::entity entity(const Json& id) const {
        return static_cast<entt::entity>(id.get<std::uint32_t>());
    }
    [[nodiscard]] entt::registry& reg() { return scene.registry(); }
};

} // namespace quat::test
