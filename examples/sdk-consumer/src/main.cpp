// sdk-consumer game: a headless run of the module against the installed SDK.
// Builds a scene with spinners and a falling physics body, steps it, queries
// it through the remote-method table and checks the data root quat_add_game()
// laid out next to the executable. Exit code 0 = everything worked.

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/version.hpp"
#include "spinner.hpp"

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "sdk_consumer_game: %s\n", what);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    int frames = 60;
    if (argc == 3 && std::string_view(argv[1]) == "--frames") {
        const std::string_view n(argv[2]);
        if (std::from_chars(n.data(), n.data() + n.size(), frames).ec != std::errc{} || frames < 1) {
            return fail("--frames needs a positive integer");
        }
    } else if (argc != 1) {
        return fail("usage: sdk_consumer_game [--frames N]");
    }
    std::printf("quat3d %s\n", quat3d::version_string());

    // quat_add_game() made the executable's directory a data root.
    const std::filesystem::path exe_dir = std::filesystem::absolute(argv[0]).parent_path();
    if (!std::filesystem::is_directory(exe_dir / "cooked" / "shaders") ||
        std::filesystem::is_empty(exe_dir / "cooked" / "shaders")) {
        return fail("no cooked engine shaders next to the executable");
    }
    if (!std::filesystem::is_regular_file(exe_dir / "assets" / "scenes" / "spinners.json")) {
        return fail("the game's assets were not copied next to the executable");
    }

    engine::scene::Scene scene;
    for (int i = 0; i < 3; ++i) {
        const entt::entity e = scene.create_entity("spinner" + std::to_string(i));
        scene.registry().emplace<consumer::Spinner>(e, consumer::Spinner{45.0F * static_cast<float>(i + 1), 0.0F});
    }
    const entt::entity ball = scene.create_entity("ball");
    scene.registry().get<engine::scene::Transform>(ball).local[3] = glm::vec4(0.0F, 10.0F, 0.0F, 1.0F);
    scene.registry().emplace<engine::scene::Collider>(
        ball, engine::scene::Collider{engine::scene::ColliderShape::sphere, glm::vec3(0.5F), glm::vec3(0.0F), false});
    scene.registry().emplace<engine::scene::RigidBody>(ball);

    auto world = engine::physics::PhysicsWorld::create();
    if (!world) return fail("PhysicsWorld::create failed");

    engine::remote::RpcServer server;
    consumer::register_spinner_methods(server, scene);

    constexpr float dt = 1.0F / 60.0F;
    for (int f = 0; f < frames; ++f) {
        consumer::spinner_system(scene.registry(), dt);
        engine::scene::physics_system(scene.registry(), *world, dt);
        scene.tick(dt);
    }

    const float ball_y = scene.registry().get<engine::scene::Transform>(ball).world[3].y;
    if (!(ball_y < 10.0F)) return fail("the physics body did not fall");
    auto listed = server.call("consumer.spinners", engine::remote::Json::object());
    if (!listed || listed->at("count") != 3) return fail("consumer.spinners did not list 3 spinners");
    std::printf("ball y %.3f after %d frames; spinners %s\n", static_cast<double>(ball_y), frames,
                listed->at("angles").dump().c_str());
    return 0;
}
