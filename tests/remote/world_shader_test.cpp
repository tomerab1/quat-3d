// Material shader commands (ROADMAP A6): shader.list / get / template /
// delete through RpcServer::call, and the deferred shader.create / update over
// a loopback connection with a real compile (skipped without slangc): the
// reply carries the outcome with located diagnostics, and a failed update
// keeps the previous version.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "engine/rhi/material_shader_registry.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "remote/socket.hpp"
#include "remote/world_fixture.hpp"
#include "support/expect_ok.hpp"
#include "support/material_shader_tree.hpp"

namespace {

namespace fs = std::filesystem;
using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

TEST(ShaderCommandsNoRegistry, AreUnavailable) {
    s::Scene scene;
    engine::remote::RpcServer server;
    engine::remote::register_world_commands(server, scene, {});
    for (const char* method : {"shader.list", "shader.template"}) {
        const auto r = server.call(method, Json::object());
        ASSERT_FALSE(r) << method;
        EXPECT_EQ(r.error().code, code::unavailable);
    }
    const auto r = server.call("shader.get", {{"name", "standard"}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, code::unavailable);
}

// ---- Synchronous methods, registry without a compiler ---------------------------

class ShaderCommands : public quat::test::WorldCommands {
protected:
    void SetUp() override {
        write_file(tree.materials / "lava.slang", quat::test::test_material("flat_glow.slang"));
        registry.emplace(tree.registry_config(), nullptr);
        services.material_shaders = &*registry;
        register_commands();
    }
    quat::test::MaterialShaderTree                     tree{"shader_commands"};
    std::optional<engine::rhi::MaterialShaderRegistry> registry;
};

TEST_F(ShaderCommands, ListGetAndTemplate) {
    const Json list = ok("shader.list");
    EXPECT_EQ(list["can_compile"], false);
    ASSERT_EQ(list["shaders"].size(), 2U);
    EXPECT_EQ(list["shaders"][0]["name"], "standard");
    EXPECT_EQ(list["shaders"][0]["builtin"], true);
    EXPECT_EQ(list["shaders"][1]["name"], "lava");
    EXPECT_EQ(list["shaders"][1]["path"], "assets/shaders/materials/lava.slang");
    EXPECT_EQ(list["shaders"][1]["state"], "not_compiled");

    const Json lava = ok("shader.get", {{"name", "lava"}});
    EXPECT_EQ(lava["source"], quat::test::test_material("flat_glow.slang"));
    EXPECT_TRUE(lava["diagnostics"].is_array());
    const Json builtin = ok("shader.get", {{"name", "standard"}});
    EXPECT_TRUE(contains(builtin["source"].get<std::string>(), "interface IMaterial"));

    const Json tmpl = ok("shader.template");
    EXPECT_TRUE(contains(tmpl["source"].get<std::string>(), "export struct SurfaceMaterial"));

    EXPECT_TRUE(contains(err("shader.get", {{"name", "nope"}}, code::not_found), "nope"));
    EXPECT_TRUE(contains(err("shader.get", {{"name", "../lava"}}, code::invalid_params), "name"));
    EXPECT_TRUE(contains(err("shader.get", Json::object(), code::invalid_params), "name"));
    err("shader.get", {{"name", "lava"}, {"extra", 1}}, code::invalid_params);
}

TEST_F(ShaderCommands, DeleteResetsTheEntitiesUsingIt) {
    const entt::entity a = scene.create_entity("a");
    reg().emplace<s::Primitive>(a).shader = "lava";
    const entt::entity b = scene.create_entity("b");
    reg().emplace<s::InstancedMesh>(b).primitive.shader = "lava";
    const entt::entity c = scene.create_entity("c");
    reg().emplace<s::Primitive>(c).shader = "other";

    const Json r = ok("shader.delete", {{"name", "lava"}});
    EXPECT_EQ(r["deleted"], "lava");
    EXPECT_EQ(r["entities_reset"], 2);
    EXPECT_EQ(reg().get<s::Primitive>(a).shader, "");
    EXPECT_EQ(reg().get<s::InstancedMesh>(b).primitive.shader, "");
    EXPECT_EQ(reg().get<s::Primitive>(c).shader, "other");
    EXPECT_FALSE(fs::exists(tree.materials / "lava.slang"));

    err("shader.delete", {{"name", "lava"}}, code::not_found);
    err("shader.delete", {{"name", "standard"}}, code::invalid_params);
}

TEST_F(ShaderCommands, CreateNeedsTheCompilerAndADeferredCall) {
    // RpcServer::call cannot complete deferred methods; over a connection the
    // registry answers `unavailable` without a compiler (see below).
    err("shader.create", {{"name", "x"}, {"source", "import material;"}}, code::unavailable);
}

// ---- Deferred create / update over a real connection ------------------------------

class ShaderCompileCommands : public ::testing::Test {
protected:
    void SetUp() override {
        if (!engine::rhi::find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";
        ASSERT_TRUE(tree.ready()) << tree.error;
        auto reload = engine::rhi::ShaderHotReload::create(cache, tree.reload_config());
        ASSERT_OK(reload);
        hot_reload = std::move(*reload);
        registry.emplace(tree.registry_config(), hot_reload.get());

        engine::remote::RpcServerConfig config;
        config.port = 0;
        auto listened = engine::remote::RpcServer::listen(config);
        ASSERT_OK(listened);
        server.emplace(std::move(*listened));
        engine::remote::WorldServices services;
        services.material_shaders = &*registry;
        engine::remote::register_world_commands(*server, scene, std::move(services));
        auto c = engine::remote::detail::connect_loopback(server->port());
        ASSERT_OK(c);
        sock = std::move(*c);
    }

    void send(const Json& msg) {
        const std::string line = msg.dump() + "\n";
        std::size_t off = 0;
        for (int spins = 0; off < line.size() && spins < 1000; ++spins) {
            const auto r = engine::remote::detail::send_some(sock, line.data() + off,
                                                             line.size() - off);
            if (r.status == engine::remote::detail::IoStatus::ok) off += r.bytes;
        }
        ASSERT_EQ(off, line.size());
    }

    // Runs "frames" (server poll, compile service, registry) until a reply
    // arrives or `ms` pass.
    Json pump(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto nl = inbuf.find('\n'); nl != std::string::npos) {
                const std::string line = inbuf.substr(0, nl);
                inbuf.erase(0, nl + 1);
                return Json::parse(line, nullptr, false);
            }
            server->poll();
            hot_reload->update(frame++);
            registry->poll();
            char buf[65536];
            const auto r = engine::remote::detail::recv_some(sock, buf, sizeof(buf));
            if (r.status == engine::remote::detail::IoStatus::ok) {
                inbuf.append(buf, r.bytes);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return nullptr;
    }

    Json request(int id, const std::string& method, const Json& params) {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        return pump(30000);
    }

    quat::test::MaterialShaderTree                     tree{"shader_compile_commands"};
    engine::rhi::PipelineCache                         cache;
    std::unique_ptr<engine::rhi::ShaderHotReload>      hot_reload;
    std::optional<engine::rhi::MaterialShaderRegistry> registry;
    s::Scene                                           scene;
    std::optional<engine::remote::RpcServer>           server;
    engine::remote::detail::Socket                     sock;
    std::string                                        inbuf;
    std::uint64_t                                      frame = 1;
};

TEST_F(ShaderCompileCommands, CreateAndUpdateReplyWithTheCompileOutcome) {
    const std::string good = quat::test::test_material("flat_glow.slang");
    const Json created = request(1, "shader.create", {{"name", "glow"}, {"source", good}});
    ASSERT_TRUE(created.contains("result")) << created.dump();
    EXPECT_EQ(created["result"]["ok"], true) << created.dump();
    EXPECT_EQ(created["result"]["errors"], 0);
    EXPECT_EQ(created["result"]["path"], "assets/shaders/materials/glow.slang");

    const Json bad = request(2, "shader.update",
                             {{"name", "glow"}, {"source", quat::test::test_material("broken.slang")}});
    ASSERT_TRUE(bad.contains("result")) << bad.dump();
    const Json& r = bad["result"];
    EXPECT_EQ(r["ok"], false);
    EXPECT_EQ(r["kept_previous"], true);
    EXPECT_EQ(r["errors"], 1);
    ASSERT_FALSE(r["diagnostics"].empty());
    const Json& d = r["diagnostics"][0];
    EXPECT_EQ(d["severity"], "error");
    EXPECT_EQ(d["file"], "assets/shaders/materials/glow.slang");
    EXPECT_EQ(d["line"], 9);
    EXPECT_EQ(d["column"], 33);
    EXPECT_TRUE(contains(d["message"].get<std::string>(), "uvv"));
    EXPECT_TRUE(contains(d["source_line"].get<std::string>(), "input.uvv"));

    // Reading it back gives the version that is still running.
    const Json got = request(3, "shader.get", {{"name", "glow"}});
    EXPECT_EQ(got["result"]["source"], good);

    const Json dup = request(4, "shader.create", {{"name", "glow"}, {"source", good}});
    ASSERT_TRUE(dup.contains("error")) << dup.dump();
    EXPECT_EQ(dup["error"]["code"], code::failed);
    EXPECT_TRUE(contains(dup["error"]["message"].get<std::string>(), "shader.update"));

    const Json missing = request(5, "shader.update", {{"name", "nope"}, {"source", good}});
    EXPECT_EQ(missing["error"]["code"], code::not_found);
    const Json no_source = request(6, "shader.create", {{"name", "x"}});
    EXPECT_EQ(no_source["error"]["code"], code::invalid_params);
}

} // namespace
