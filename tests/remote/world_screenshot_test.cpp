// render.screenshot with an explicit camera (issue #41): the request carries
// the camera to render from, and the viewport camera stays where the user
// left it. render.screenshot is deferred, so it runs over a loopback socket.

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/remote/rpc_server.hpp"
#include "engine/remote/world_commands.hpp"
#include "engine/scene/scene.hpp"
#include "remote/socket.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::remote::CameraPose;
using engine::remote::Json;
using engine::remote::ScreenshotRequest;

class ScreenshotCamera : public ::testing::Test {
protected:
    engine::scene::Scene                     scene;
    std::optional<engine::remote::RpcServer> server;
    engine::remote::detail::Socket           sock;
    std::string                              inbuf;
    CameraPose                               viewport{{0, 2, 10}, -90.0F, -10.0F, 60.0F};
    int                                      camera_sets = 0;
    std::vector<ScreenshotRequest>           requests;
    int                                      next_id = 1;

    void SetUp() override {
        engine::remote::RpcServerConfig config;
        config.port = 0;
        auto listened = engine::remote::RpcServer::listen(config);
        ASSERT_OK(listened);
        server.emplace(std::move(*listened));
        engine::remote::WorldServices s;
        s.camera_get = [this] { return viewport; };
        s.camera_set = [this](const CameraPose& p) {
            viewport = p;
            ++camera_sets;
        };
        // The application would render the frame from request.camera; the
        // fake records the request and answers at once.
        s.screenshot = [this](const ScreenshotRequest& r, engine::remote::Responder responder) {
            requests.push_back(r);
            responder.reply(Json{{"mime", "image/png"}, {"width", 1}, {"height", 1}, {"data", ""}});
        };
        engine::remote::register_world_commands(*server, scene, std::move(s));
        auto c = engine::remote::detail::connect_loopback(server->port());
        ASSERT_OK(c);
        sock = std::move(*c);
    }

    // Sends one request and returns its reply (null on timeout).
    Json rpc(const std::string& method, const Json& params) {
        const Json msg{{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", method}, {"params", params}};
        const std::string line = msg.dump() + "\n";
        std::size_t off = 0;
        for (int spins = 0; off < line.size() && spins < 1000; ++spins) {
            const auto r =
                engine::remote::detail::send_some(sock, line.data() + off, line.size() - off);
            if (r.status == engine::remote::detail::IoStatus::ok) off += r.bytes;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto nl = inbuf.find('\n'); nl != std::string::npos) {
                const std::string reply = inbuf.substr(0, nl);
                inbuf.erase(0, nl + 1);
                return Json::parse(reply, nullptr, false);
            }
            server->poll();
            char buf[4096];
            const auto r = engine::remote::detail::recv_some(sock, buf, sizeof(buf));
            if (r.status == engine::remote::detail::IoStatus::ok) {
                inbuf.append(buf, r.bytes);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return nullptr;
    }
};

TEST_F(ScreenshotCamera, ExplicitCameraLeavesTheViewportAlone) {
    const Json before = rpc("camera.get", Json::object());
    ASSERT_TRUE(before.contains("result")) << before.dump();

    Json reply = rpc("render.screenshot",
                     {{"max_size", 256},
                      {"camera", {{"position", {50, 30, 50}}, {"look_at", {0, 0, 0}}, {"fov_deg", 40}}}});
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    ASSERT_EQ(requests.size(), 1U);
    const ScreenshotRequest& r = requests[0];
    EXPECT_EQ(r.max_size, 256U);
    ASSERT_TRUE(r.camera.has_value());
    EXPECT_EQ(r.camera->position, glm::vec3(50, 30, 50));
    EXPECT_NEAR(r.camera->yaw_deg, -135.0F, 1e-3F) << "aimed at the origin";
    EXPECT_NEAR(r.camera->pitch_deg, -glm::degrees(std::atan(30.0F / std::sqrt(5000.0F))), 1e-3F);
    EXPECT_FLOAT_EQ(r.camera->fov_deg, 40.0F);
    EXPECT_TRUE(r.camera->cut);

    // The viewport camera was neither written nor moved.
    EXPECT_EQ(camera_sets, 0);
    EXPECT_EQ(rpc("camera.get", Json::object())["result"], before["result"]);

    // Fields the camera omits keep the viewport's values.
    reply = rpc("render.screenshot", {{"camera", {{"yaw_deg", 0}, {"near", 0.01}}}});
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_EQ(requests.back().camera->position, viewport.position);
    EXPECT_FLOAT_EQ(requests.back().camera->fov_deg, 60.0F);
    EXPECT_FLOAT_EQ(*requests.back().camera->near_override, 0.01F);

    // Without a camera the viewport is captured.
    reply = rpc("render.screenshot", Json::object());
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_FALSE(requests.back().camera.has_value());
    EXPECT_EQ(requests.back().max_size, 1024U);
    EXPECT_EQ(camera_sets, 0);
}

TEST_F(ScreenshotCamera, FramesAnEntity) {
    ASSERT_TRUE(rpc("scene.create", {{"name", "tower"},
                                     {"components",
                                      {{"transform", {{"position", {100, 5, 0}}, {"scale", {2, 10, 2}}}},
                                       {"primitive", {{"shape", "cube"}}}}}})
                    .contains("result"));
    const Json reply = rpc("render.screenshot", {{"camera", {{"frame", "tower"}}}});
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    const CameraPose& p = *requests.back().camera;
    const float yaw = glm::radians(p.yaw_deg);
    const float pitch = glm::radians(p.pitch_deg);
    const glm::vec3 fwd(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                        std::cos(pitch) * std::sin(yaw));
    EXPECT_GT(glm::dot(fwd, glm::normalize(glm::vec3(100, 5, 0) - p.position)), 0.999F);
    EXPECT_EQ(camera_sets, 0);
}

TEST_F(ScreenshotCamera, BadCamerasAreRefused) {
    for (const Json& camera :
         {Json{{"look_at", {0, 0, 0}}, {"yaw_deg", 10}}, Json{{"fov_deg", 0}},
          Json{{"zoom", 2}}, Json{{"frame", "tower"}, {"position", {0, 1, 0}}},
          Json{{"near", 5}, {"far", 1}}}) {
        const Json reply = rpc("render.screenshot", {{"camera", camera}});
        ASSERT_TRUE(reply.contains("error")) << camera.dump();
        EXPECT_EQ(reply["error"]["code"], engine::remote::rpc_code::invalid_params) << reply.dump();
        EXPECT_NE(reply["error"]["message"].get<std::string>().find("camera."), std::string::npos)
            << reply.dump();
    }
    const Json missing = rpc("render.screenshot", {{"camera", {{"frame", "nobody"}}}});
    EXPECT_EQ(missing["error"]["code"], engine::remote::rpc_code::not_found) << missing.dump();
    EXPECT_TRUE(requests.empty());
}

} // namespace
