#include "app/input_runtime.hpp"

#include <system_error>
#include <utility>

#include "engine/core/log.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/camera_rig.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

namespace {

using engine::core::LogLevel;

} // namespace

void InputRuntime::init(const std::filesystem::path& project_root,
                        std::optional<std::filesystem::path> user_overrides_path) {
    engine::input::ActionMap map = engine::input::default_action_map();
    const std::filesystem::path project_map = project_root / "assets" / "input" / "actions.json";
    std::error_code ec;
    if (std::filesystem::exists(project_map, ec)) {
        auto loaded = engine::input::load_action_map_file(project_map);
        if (!loaded) {
            engine::core::log(LogLevel::error, "input", "{} (using the default action map)", loaded.error().message);
        } else if (auto ok = engine::input::merge_action_map(map, std::move(*loaded)); !ok) {
            engine::core::log(LogLevel::error, "input", "{}: {}", engine::core::path_to_utf8(project_map),
                              ok.error().message);
        } else {
            engine::core::log(LogLevel::info, "input", "action map {} ({} actions)",
                              engine::core::path_to_utf8(project_map), map.actions.size());
        }
    }
    actions_.set_map(std::move(map));
    user_path_ = std::move(user_overrides_path);
    if (user_path_) {
        if (auto ok = engine::input::load_overrides_file(actions_, *user_path_); !ok) {
            engine::core::log(LogLevel::error, "input", "{} (rebinding ignored)", ok.error().message);
        }
    }
}

std::expected<void, engine::core::Error> InputRuntime::save_overrides() const {
    if (!user_path_) return std::unexpected(engine::core::Error{"no per-user input file on this platform"});
    return engine::input::save_overrides_file(actions_, *user_path_);
}

void InputRuntime::update(const engine::input::DeviceState& device, float dt, float camera_yaw_rad) {
    device_ = device;
    actions_.set_camera_yaw(camera_yaw_rad);
    actions_.update(device, dt);
}

void InputRuntime::update_rigs(engine::scene::Scene& scene, const engine::scene::GroundHeightFn& ground,
                               const engine::physics::PhysicsWorld* physics, bool rig_input, float dt) {
    engine::scene::RigEnvironment env;
    if (ground) env.ground = ground;
    if (physics != nullptr) {
        env.raycast = [physics](const glm::vec3& o, const glm::vec3& d, float max) -> std::optional<glm::vec3> {
            const auto hit = physics->cast_ray(o, d, max);
            if (!hit) return std::nullopt;
            return hit->point;
        };
    }
    env.actions = rig_input ? &actions_ : nullptr;
    env.pointer = device_.pointer();
    env.viewport_px = glm::max(device_.viewport_size, glm::vec2(1.0F));
    engine::scene::camera_rig_system(scene.registry(), dt, env);
}

} // namespace quat::app
