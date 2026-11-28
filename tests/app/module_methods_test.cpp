// Game-module remote methods on the endpoint (ROADMAP G17.1): parameters are
// validated against the method's JSON Schema before the handler runs,
// rpc.methods lists the schemas, and an existing method is never replaced.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app/module_methods.hpp"
#include "app/module_runtime.hpp"
#include "engine/game/module_set.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

using nlohmann::json;
namespace game = engine::game;
namespace remote = engine::remote;

std::expected<void, engine::core::Error> build_module(game::Registrar& r) {
    r.method({.name = "build.place",
              .summary = "Place a thing",
              .params_schema = json::parse(R"({"type": "object", "required": ["kind"],
                  "properties": {"kind": {"enum": ["wall", "tower"]}}, "additionalProperties": false})"),
              .result_schema = json::parse(R"({"type": "object"})"),
              .handler = [](game::SystemContext& ctx, const json& p) -> std::expected<json, engine::core::Error> {
                  if (p["kind"] == "tower") return std::unexpected(engine::core::Error{"towers are not built yet"});
                  const entt::entity e = ctx.scene().create_entity("wall");
                  return json{{"entity", entt::to_integral(e)}, {"frame", ctx.frame()}};
              }});
    r.method({.name = "build.ping", .summary = "taken", .handler = [](game::SystemContext&, const json&)
                  -> std::expected<json, engine::core::Error> { return json("module"); }});
    return {};
}

TEST(ModuleMethods, ValidateDispatchAndList) {
    engine::scene::ComponentRegistry components;
    const game::ModuleEntry modules[] = {{"builder", &build_module}};
    auto set = game::ModuleSet::load(modules, components, {});
    ASSERT_OK(set);
    quat::app::ModuleRuntime runtime;
    runtime.bind(&*set, nullptr, {});
    engine::scene::Scene scene;
    remote::RpcServer server;
    server.add_method("build.ping", "the engine's", [](const remote::Json&) -> remote::RpcResult {
        return remote::Json("engine");
    });
    const std::uint64_t frame = 42;
    quat::app::install_module_methods(server, runtime, scene, nullptr, &frame);

    auto placed = server.call("build.place", {{"kind", "wall"}});
    ASSERT_TRUE(placed) << placed.error().message;
    EXPECT_EQ((*placed)["frame"], 42);
    EXPECT_EQ(scene.registry().storage<entt::entity>().size(), 1U);

    auto invalid = server.call("build.place", {{"kind", "moat"}});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, remote::rpc_code::invalid_params);
    EXPECT_NE(invalid.error().message.find("params.kind"), std::string::npos) << invalid.error().message;

    auto failed = server.call("build.place", {{"kind", "tower"}});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, remote::rpc_code::failed);
    EXPECT_EQ(failed.error().message, "towers are not built yet");

    EXPECT_EQ(*server.call("build.ping", remote::Json::object()), "engine") << "never replaced";

    auto listed = server.call("rpc.methods", remote::Json::object());
    ASSERT_TRUE(listed);
    bool found = false;
    for (const remote::Json& m : *listed) {
        if (m["name"] != "build.place") continue;
        found = true;
        EXPECT_EQ(m["params_schema"]["required"], json::array({"kind"}));
        EXPECT_EQ(m["result_schema"]["type"], "object");
    }
    EXPECT_TRUE(found);
}

} // namespace
