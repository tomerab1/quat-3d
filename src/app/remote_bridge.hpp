#pragma once

// RemoteBridge — the `game` executable's side of remote control (ROADMAP
// A2/A3, app layer). It owns the JSON-RPC endpoint (QUAT_REMOTE=<port>,
// QUAT_REMOTE_TOKEN), registers engine.info plus the world commands, and
// implements their WorldServices against the frame loop's state: the fly
// camera, clouds, terrain heights, glTF spawning, play mode, scene files,
// navmesh baking and screenshots.
//
// It also resolves Primitive components every frame (independent of the
// endpoint — scene files carry primitives too), opening an upload context
// only when some primitive changed. Likewise for R9.2 instancing: it runs
// scatter_system against the terrain the frame loop holds (streamed tiles or
// the editing tile) and resolves InstancedMesh models.
//
// It owns the simulation clock (ROADMAP A5: debug.pause / step / time_scale)
// and the frame statistics debug.stats reports.
//
// Frame-loop hooks (main thread):
//   poll()               once per frame before the scene updates: RPC
//                        requests, primitive + instanced-mesh resolution,
//                        scatter regeneration, finished screenshots,
//                        completed debug.step requests
//   advance_sim(dt)      once per frame with the real frame time: records it
//                        and returns the simulation dt (0 while paused);
//                        advance_sim_unrendered() instead on iterations that
//                        draw nothing (minimised)
//   note_draws(n)        the frame's submitted draw count (debug.stats)
//   record_capture()     after the frame's graph is recorded: copies the
//                        final scene image for pending screenshots
//   on_frame_complete()  right after the frame slot's fence wait: hands the
//                        slot's readback to a background PNG encoder
//
// Screenshots never stall the GPU: the copy lands in a per-slot host-visible
// buffer that is read only once that slot's fence has signalled anyway, and
// PNG encoding runs on a worker thread; the reply goes out from poll()
// (screenshot_capture.hpp).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "app/screenshot_capture.hpp" // CaptureSource
#include "engine/core/stderr_capture.hpp"
#include "engine/scene/scene.hpp" // GroundHeightFn

namespace engine::asset {
class AssetManager;
}
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
class Device;
class GpuAllocator;
} // namespace engine::rhi
namespace engine::scene {
class MaterialOverrideStore;
}
namespace engine::terrain {
class TerrainStreamer;
struct Heightmap;
} // namespace engine::terrain
#ifdef ENGINE_EDITOR
namespace engine::editor {
class EditorLayer;
} // namespace engine::editor
#endif

namespace quat::app {

// Frame-loop state the bridge reads and drives. Non-owning: main() owns all of
// it and keeps it alive for the bridge's lifetime.
struct RemoteBridgeTargets {
    engine::rhi::Device*        device = nullptr;
    engine::rhi::GpuAllocator*  allocator = nullptr;
    engine::asset::AssetManager* assets = nullptr;
    engine::scene::Scene*       scene = nullptr;
    const std::uint64_t*        frame_counter = nullptr; // presented frames

    // The view (free-fly) camera; yaw/pitch/fov in radians.
    glm::vec3* cam_pos = nullptr;
    float*     cam_yaw = nullptr;
    float*     cam_pitch = nullptr;
    float*     cam_fov_y = nullptr;

    const engine::scene::GroundHeightFn* ground_height = nullptr;

    // Navmesh bake inputs and output.
    const std::optional<engine::terrain::TerrainStreamer>* terrain_streamer = nullptr;
    const engine::terrain::Heightmap* terrain_heightmap = nullptr;
    const glm::vec3*                  terrain_anchor = nullptr;
    engine::nav::NavMesh*             navmesh = nullptr;

    // Physics worlds the debug and physics.* methods use: the demo modes' world
    // and the editor's play-mode world (whichever is engaged).
    std::optional<engine::physics::PhysicsWorld>* demo_physics = nullptr;
    std::optional<engine::physics::PhysicsWorld>* play_physics = nullptr;

    // Loop-cached entity dropped when a scene file replaces the scene.
    entt::entity* spin_entity = nullptr;

    // The renderer's settings (render.settings.*, environment.* clouds,
    // scene files).
    engine::renderer::RenderSettings* render_settings = nullptr;
    // The renderer's per-entity derived materials (A8), resolved in poll().
    engine::scene::MaterialOverrideStore* material_overrides = nullptr;

#ifdef ENGINE_EDITOR
    engine::editor::EditorLayer* editor = nullptr;
    bool* play = nullptr; // editor play flag; null where play mode is unavailable
#endif

    std::filesystem::path project_root;
};

class RemoteBridge {
public:
    // Opens the endpoint when QUAT_REMOTE is set (failure is logged, the
    // bridge still resolves primitives). `frames_in_flight` sizes the
    // per-slot readback buffers.
    RemoteBridge(RemoteBridgeTargets targets, std::uint32_t frames_in_flight);
    RemoteBridge(RemoteBridge&&) noexcept;
    RemoteBridge& operator=(RemoteBridge&&) noexcept;
    RemoteBridge(const RemoteBridge&) = delete;
    RemoteBridge& operator=(const RemoteBridge&) = delete;
    ~RemoteBridge();

    void poll();
    void record_capture(VkCommandBuffer cmd, std::uint32_t frame_slot, const CaptureSource& source);
    void on_frame_complete(std::uint32_t frame_slot);

    // Simulation dt for this frame given the real frame time `real_dt`.
    [[nodiscard]] float advance_sim(float real_dt);
    // Simulation dt for a loop iteration that draws no frame (minimised
    // window): the next queued debug.step step, else 0 — real-time simulation
    // stays frozen, but stepping callers still get their reply.
    [[nodiscard]] float advance_sim_unrendered();
    void note_draws(std::size_t draws);

    [[nodiscard]] bool listening() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Tees the process's stderr into the engine log ring (debug.log) when the
// remote-control endpoint is requested (QUAT_REMOTE set) unless
// QUAT_LOG_CAPTURE=0. Keep the returned object alive for the whole run.
[[nodiscard]] engine::core::StderrCapture capture_stderr_for_remote();

} // namespace quat::app
