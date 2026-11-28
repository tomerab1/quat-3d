#pragma once

// World commands (ROADMAP A2/A3) — the remote-control methods that let an
// out-of-process tool (the quat-mcp bridge, i.e. a language model) inspect and
// build the world: scene.*, environment.*, world.ground_height, camera.*,
// render.screenshot, asset.*, sim.*, scene.save/load, nav.bake,
// terrain.status, world.place_on_ground — the
// debugging commands (ROADMAP A5): debug.log, debug.stats, debug.pause /
// resume / step / time_scale, debug.watch, debug.raycast, debug.nav_path —
// look tuning (ROADMAP A8): render.settings.get / set / reset / schema,
// material.get / set — weather (ROADMAP R13.1): weather.get / set —
// behaviour-tree authoring (ROADMAP A7): bt.leaves, bt.get, bt.set,
// bt.add_node, bt.remove_node, bt.move_node, bt.set_params, bt.status — the
// gameplay script files (ROADMAP G7): script.list / get / create / update /
// delete / check — material shader authoring (ROADMAP A6): shader.list /
// get / template / create / update / delete — game packaging (ROADMAP P4):
// project.info, project.build, project.build_status — the world clock
// (ROADMAP R22.1): time.get / set / skip — and profiling (ROADMAP R4):
// profile.frame / capture / result / counters / reset. The method contract is
// docs/remote-control.md.
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
#include <string>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/project/build_job.hpp"
#include "engine/project/game_project.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/renderer/physical_camera.hpp"
#include "engine/weather/wind_field.hpp"

namespace engine::core {
class EventRing;
class FrameProfiler;
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
namespace engine::rhi {
class MaterialShaderRegistry;
}
namespace engine::scene {
class Scene;
}
namespace engine::script {
class ScriptRuntime;
}

namespace engine::remote {

// The view camera (the editor's fly camera). yaw_deg is the heading in the XZ
// plane measured from +X toward +Z (forward = (cos pitch cos yaw, sin pitch,
// cos pitch sin yaw)); pitch_deg is positive looking up.
//
// Clip planes: near_m / far_m are the planes in effect (camera_get). They are
// derived from the scene (drawables + terrain, engine/scene/view_clip.hpp)
// unless overridden: near_override / far_override (nullopt = derived) are
// what camera_set stores.
//
// `cut` (camera_set only): the move is a teleport, so temporal history (TAA)
// must restart; false for continuous motion driven by repeated calls.
struct CameraPose {
    glm::vec3 position{0.0F};
    float     yaw_deg = 0.0F;
    float     pitch_deg = 0.0F;
    float     fov_deg = 60.0F; // vertical field of view
    float     near_m = 0.1F;
    float     far_m = 1000.0F;
    std::optional<float> near_override{};
    std::optional<float> far_override{};
    bool      cut = false;
    // R20 physical lens of the view (exposure, depth of field, shutter); a
    // focal length > 0 overrides fov_deg.
    renderer::PhysicalCamera lens{};
};

// Frame timing over the last `samples` frames and renderer counters, for
// debug.stats. Optional fields are omitted when the application lacks them.
struct FrameStats {
    std::uint64_t                frame = 0; // frames presented so far
    float                        last_ms = 0.0F;
    float                        avg_ms = 0.0F;
    float                        worst_ms = 0.0F;
    std::uint32_t                samples = 0;
    std::optional<std::uint32_t> draws;            // scene draws last frame, after batching
    std::optional<std::uint64_t> resident_bytes;   // process resident memory
};

// The game project the engine opened (QUAT_GAME, or the player's game.json).
struct GameProjectInfo {
    std::filesystem::path root; // absolute; holds game.json
    project::GameProject  project;
};

// A validated project.build request.
struct ProjectBuildParams {
    std::string            target; // empty: the host's
    std::filesystem::path  output; // absolute, inside the game project
    project::PackageConfig config = project::PackageConfig::shipping;
    bool                   smoke_test = false;
};

// The lighting environment's HDRI panorama (ROADMAP R21), as environment.get
// reports it.
struct HdriInfo {
    std::string   path; // project-relative; empty = none
    bool          loaded = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool          sun_found = false;
    std::string   error; // why the last load failed
};

// Terrain generation state (terrain.status). Tile counts and bounds are those
// of the runtime's terrain (the first Terrain entity's); bounds are relative
// to its anchor (the entity's world position), like terrain_height.
struct TerrainStatus {
    bool          generating = false;  // a tile is being generated / uploaded
    std::uint32_t tiles_loaded = 0;
    std::uint32_t tiles_wanted = 0;    // 1, or the streaming window's (2r+1)²
    bool          has_bounds = false;  // some tile is loaded
    glm::vec3     bounds_min{0.0F};    // loaded tiles, anchor-relative
    glm::vec3     bounds_max{0.0F};
};

// A render.screenshot request. With `camera`, the application renders one
// frame from that view (instead of the viewport camera, which it leaves
// untouched) and captures that frame.
struct ScreenshotRequest {
    std::uint32_t             max_size = 1024; // longer side of the reply, pixels
    std::optional<CameraPose> camera;
};

// One GPU memory heap for profile.counters (bytes; see rhi::GpuHeapStats).
struct GpuMemoryHeap {
    std::uint32_t heap = 0;
    bool          device_local = false;
    std::uint64_t size = 0;
    std::uint64_t budget = 0;
    std::uint64_t usage = 0;
    std::uint64_t allocated = 0; // memory blocks the engine's allocator holds
    std::uint64_t used = 0;      // of which bound to resources
    std::uint32_t allocations = 0;
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

    // Height of the loaded terrain above its anchor at anchor-relative (x, z)
    // (tile (0,0) is centred on the anchor); nullopt where no tile is loaded.
    // The world commands place the anchor from the Terrain entity's current
    // transform, so edits earlier in a batch apply at once (issue #39).
    std::function<std::optional<float>(float local_x, float local_z)> terrain_height;
    std::function<TerrainStatus()>                                    terrain_status;

    // Instantiate a glTF/GLB (absolute path, already confined to the project
    // root) into the scene; returns the root entities created.
    std::function<std::expected<std::vector<entt::entity>, core::Error>(
        const std::filesystem::path& absolute)>
        spawn_model;

    // Capture the next rendered frame (from request.camera when given),
    // downscaled so its longer side is at most request.max_size, and reply
    // {mime, width, height, data} later.
    std::function<void(const ScreenshotRequest& request, Responder responder)> screenshot;

    std::function<bool()>                                    playing;
    std::function<std::expected<bool, core::Error>(bool play)> set_playing;

    std::function<std::expected<void, core::Error>(const std::filesystem::path& absolute)>
        save_scene;
    std::function<std::expected<void, core::Error>(const std::filesystem::path& absolute)>
        load_scene;

    // Bake the navmesh from the current static world; returns its polygon count.
    std::function<std::expected<std::size_t, core::Error>()> nav_bake;

    // ---- Lighting environment (ROADMAP R21) --------------------------------------
    // The HDRI panorama's status, and loading one: `relative` is a project-
    // relative path already confined to the project root ("" clears it).
    std::function<HdriInfo()>                                                hdri_info;
    std::function<std::expected<void, core::Error>(const std::string& relative)> set_hdri;

    // ---- Debugging (ROADMAP A5) ------------------------------------------------

    // Log lines debug.log reads (non-owning; normally core::log_ring()).
    core::LogRing* log_ring = nullptr;

    // The simulation clock the frame loop advances (non-owning): debug.pause,
    // resume, step and time_scale drive it.
    core::SimClock* sim_clock = nullptr;
    // Calls `done` (on the main thread, from a later poll) once the clock's
    // steps_run reaches `steps_target` or no steps are pending any more
    // (resume drops them). Without it debug.step replies right after queueing.
    // debug.step keeps at most 64 waits outstanding (it replies busy beyond).
    std::function<void(std::uint64_t steps_target, std::function<void()> done)> when_stepped;

    // Frame timing and renderer counters for debug.stats.
    std::function<FrameStats()> frame_stats;

    // The physics world being simulated, or null (edit mode: bodies only
    // exist while playing). debug.raycast and debug.stats read it.
    std::function<const physics::PhysicsWorld*()> physics_world;
    // The same world, writable: the physics.* and character.move methods
    // (A10.1) read and drive it. Unset or null: they answer `unavailable`.
    std::function<physics::PhysicsWorld*()> physics_control;

    // Gameplay events events.poll reads (non-owning; normally
    // core::event_ring(), fed by the frame loop after each physics update).
    core::EventRing* event_ring = nullptr;

    // The current navmesh, or null / invalid when none is baked.
    std::function<const nav::NavMesh*()> navmesh;

    // ---- Weather (ROADMAP R13.1) -------------------------------------------------

    // The renderer's wind clock, so weather.get's wind_at answers match the
    // field the frame draws. Empty: a clock at zero.
    std::function<weather::WindAdvection()> wind_clock;

    // ---- Gameplay scripts (ROADMAP G7) ------------------------------------------

    // The scene's script runtime: script.* report its per-file status and
    // hot-reload files they write. Without it the file commands still work
    // (on <project_root>/assets/scripts).
    std::function<script::ScriptRuntime*()> scripts;

    // ---- Profiling (ROADMAP R4) -------------------------------------------------

    // The frame profiler (non-owning; normally core::profiler()): profile.*
    // read its statistics and run captures on it. Null: unavailable.
    core::FrameProfiler* profiler = nullptr;
    // Per-heap GPU memory for profile.counters (empty: reported as null).
    std::function<std::vector<GpuMemoryHeap>()> gpu_memory;
    // ---- Material shaders (ROADMAP A6) -------------------------------------------

    // Authored material shaders (non-owning; its poll() must run every frame
    // so shader.create / update replies go out). Null: shader.* unavailable.
    rhi::MaterialShaderRegistry* material_shaders = nullptr;

    // ---- Game projects and packaging (ROADMAP P4) -----------------------------

    // The open game project; nullopt when the engine runs without one.
    std::function<std::optional<GameProjectInfo>()> game_project;
    // Starts packaging the open project in the background (quat-pack);
    // returns the job number. Fails when a build runs or the tools are missing.
    std::function<std::expected<std::uint64_t, core::Error>(const ProjectBuildParams&)>
        project_build;
    // The packaging job's state (idle before the first build).
    std::function<project::BuildStatus()> project_build_status;
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
