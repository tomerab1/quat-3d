#pragma once

// World commands (ROADMAP A2/A3) — the remote-control methods that let an
// out-of-process tool (the quat-mcp bridge, i.e. a language model) inspect and
// build the world: scene.*, environment.*, world.ground_height, camera.*,
// render.screenshot, asset.*, sim.*, scene.save/load, nav.bake — the
// debugging commands (ROADMAP A5): debug.log, debug.stats, debug.pause /
// resume / step / time_scale, debug.watch, debug.raycast, debug.nav_path — and
// look tuning (ROADMAP A8): render.settings.get / set / reset / schema,
// material.get / set — and behaviour-tree authoring (ROADMAP A7): bt.leaves,
// bt.get, bt.set, bt.add_node, bt.remove_node, bt.move_node, bt.set_params,
// bt.status. The method contract is docs/remote-control.md.
//
// Scene editing works on any Scene. Everything that only the running
// application has — the view camera, the renderer's settings, terrain heights,
// glTF uploads, screenshots, play mode, scene files, the navmesh — is reached
// through WorldServices; a callback (or pointer) left empty makes its methods
// answer `unavailable` (tests, headless tools).
//
// All handlers run on the main thread (RpcServer::poll), so they mutate the
// ECS between frames exactly like editor actions. Requests are decoded through
// core::JsonFields in strict mode: unknown fields are errors that name the
// known ones.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/remote/rpc_server.hpp"

namespace engine::core {
class LogRing;
class SimClock;
} // namespace engine::core
namespace engine::nav {
class NavMesh;
}
namespace engine::physics {
class PhysicsWorld;
}
namespace engine::renderer {
struct RenderSettings;
}
namespace engine::scene {
class Scene;
}

namespace engine::remote {

// The view camera (the editor's fly camera). yaw_deg is the heading in the XZ
// plane measured from +X toward +Z (forward = (cos pitch cos yaw, sin pitch,
// cos pitch sin yaw)); pitch_deg is positive looking up.
struct CameraPose {
    glm::vec3 position{0.0F};
    float     yaw_deg = 0.0F;
    float     pitch_deg = 0.0F;
    float     fov_deg = 60.0F; // vertical field of view
};

// Frame timing over the last `samples` frames and renderer counters, for
// debug.stats. Optional fields are omitted when the application lacks them.
struct FrameStats {
    std::uint64_t                frame = 0; // frames presented so far
    float                        last_ms = 0.0F;
    float                        avg_ms = 0.0F;
    float                        worst_ms = 0.0F;
    std::uint32_t                samples = 0;
    std::optional<std::uint32_t> draws;            // draw items submitted last frame
    std::optional<std::uint64_t> resident_bytes;   // process resident memory
};

struct WorldServices {
    // Project root that scene.save/load, asset.list_models and
    // asset.spawn_model paths are confined to.
    std::function<std::filesystem::path()> project_root;

    std::function<CameraPose()>                  camera_get;
    std::function<void(const CameraPose& pose)>  camera_set;

    // The renderer's settings (non-owning): render.settings.* and the clouds
    // of environment.* read and write them; the next frame renders with them.
    renderer::RenderSettings* render_settings = nullptr;

    // Terrain surface height at world (x, z); nullopt where no terrain is.
    std::function<std::optional<float>(float x, float z)> ground_height;

    // Instantiate a glTF/GLB (absolute path, already confined to the project
    // root) into the scene; returns the root entities created.
    std::function<std::expected<std::vector<entt::entity>, core::Error>(
        const std::filesystem::path& absolute)>
        spawn_model;

    // Capture the next rendered frame, downscaled so its longer side is at
    // most `max_size`, and reply {mime, width, height, data} later.
    std::function<void(std::uint32_t max_size, Responder responder)> screenshot;

    std::function<bool()>                                    playing;
    std::function<std::expected<bool, core::Error>(bool play)> set_playing;

    std::function<std::expected<void, core::Error>(const std::filesystem::path& absolute)>
        save_scene;
    std::function<std::expected<void, core::Error>(const std::filesystem::path& absolute)>
        load_scene;

    // Bake the navmesh from the current static world; returns its polygon count.
    std::function<std::expected<std::size_t, core::Error>()> nav_bake;

    // ---- Debugging (ROADMAP A5) ------------------------------------------------

    // Log lines debug.log reads (non-owning; normally core::log_ring()).
    core::LogRing* log_ring = nullptr;

    // The simulation clock the frame loop advances (non-owning): debug.pause,
    // resume, step and time_scale drive it.
    core::SimClock* sim_clock = nullptr;
    // Calls `done` (on the main thread, from a later poll) once the clock's
    // steps_run reaches `steps_target` or no steps are pending any more
    // (resume drops them). Without it debug.step replies right after queueing.
    std::function<void(std::uint64_t steps_target, std::function<void()> done)> when_stepped;

    // Frame timing and renderer counters for debug.stats.
    std::function<FrameStats()> frame_stats;

    // The physics world being simulated, or null (edit mode: bodies only
    // exist while playing). debug.raycast and debug.stats read it.
    std::function<const physics::PhysicsWorld*()> physics_world;

    // The current navmesh, or null / invalid when none is baked.
    std::function<const nav::NavMesh*()> navmesh;
};

// Registers every world command on `server`. `scene` must outlive the server's
// handlers, and `server` must not be moved afterwards (scene.batch dispatches
// back through it).
void register_world_commands(RpcServer& server, scene::Scene& scene, WorldServices services);

// Resolves a client-supplied project-relative path under `root`: rejects
// empty, absolute and rooted paths and any `..` that escapes the root
// (lexically, and again after resolving symlinks of the existing prefix).
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
confine_to_root(const std::filesystem::path& root, std::string_view relative);

} // namespace engine::remote
