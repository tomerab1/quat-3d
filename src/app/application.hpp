#pragma once

// Application — the `game` executable's application layer root. Owns the
// window, the Vulkan device / allocator / surface / swapchain, the scene and
// its assets, and the subsystems that run around them:
//
//   FrameRenderer  per-frame rendering (frame slots, graph build, submission)
//   WorldRuntime   terrain, play-mode physics, nav / behaviour trees,
//                  networking, audio
//   FlyCamera      the view camera
//   EditorBridge   the editor layer and its requests (editor builds)
//   RemoteBridge   remote control (QUAT_REMOTE) and primitive resolution
//
// run() performs setup, the frame loop and teardown. Frame order: events,
// remote requests, editor requests, swapchain / frame slot, camera input,
// simulation, view, scene gather, terrain upkeep, ground clicks, navmesh bake,
// render graph, remote capture, submit + present.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <vulkan/vulkan.h>

#include "app/fly_camera.hpp"
#include "app/frame_renderer.hpp"
#include "app/remote_bridge.hpp"
#include "app/world_runtime.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "engine/rhi/swapchain.hpp"
#include "engine/scene/scene.hpp"
#ifdef ENGINE_EDITOR
#include "app/editor_bridge.hpp"
#endif

struct SDL_Window;

namespace quat::app {

// Process-level options, parsed from the environment by main().
struct AppConfig {
    // QUAT_MAX_FRAMES: auto-quit after this many presented frames (0 = run
    // until the window closes). Headless smoke tests verify clean teardown.
    std::uint64_t max_frames = 0;
    // QUAT_SCREENSHOT (with QUAT_MAX_FRAMES): the final frame as binary PPM.
    std::optional<std::string> screenshot_path;
    // False with QUAT_NO_UI=1: no editor shell (deterministic headless renders).
    bool editor_ui = true;
    // QUAT_WINDOW_SIZE=<w>x<h>: initial window size in screen coordinates
    // (headless captures of the editor want more than the 1280x720 default).
    int window_width = 1280;
    int window_height = 720;
};

class Application {
public:
    explicit Application(AppConfig config);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    // Sets up, runs the frame loop until quit, and returns the process exit
    // code (1 when setup fails; the [fatal] line says why).
    [[nodiscard]] int run();

private:
    // SDL video + the window; torn down after every other member.
    struct Platform {
        Platform() = default;
        Platform(const Platform&) = delete;
        Platform& operator=(const Platform&) = delete;
        ~Platform();
        bool        sdl_initialized = false;
        SDL_Window* window = nullptr;
    };

    [[nodiscard]] bool init_platform();
    [[nodiscard]] bool init_subsystems();
    void connect_remote();
    // Polls SDL events; false when the app should quit.
    [[nodiscard]] bool pump_events();
    // One frame; false when the loop ends (fatal error or QUAT_MAX_FRAMES).
    [[nodiscard]] bool run_frame();
    // Camera input, simulation, follow camera, then the view matrices. `dt`
    // is the real frame time, `sim_dt` the simulation's (remote debug clock).
    [[nodiscard]] engine::scene::CameraMatrices update_world(float dt, float sim_dt, float aspect);
    // A loop iteration that draws nothing (minimised window): runs the next
    // queued debug.step step, if any, through the simulation.
    void run_unrendered_step();
    void report_stats() const;

    AppConfig config_;

    Platform                   platform_;
    engine::rhi::Device        device_;
    engine::rhi::GpuAllocator  allocator_;
    engine::rhi::Swapchain     swapchain_;
    engine::rhi::PipelineCache pipeline_cache_;
    std::string                shader_dir_;
    // R2: rebuilds pipelines when assets/shaders changes (null when off).
    std::unique_ptr<engine::rhi::ShaderHotReload> shader_reload_;

#ifdef ENGINE_EDITOR
    EditorBridge editor_;
#endif
    FrameRenderer renderer_;
    // Scene assets live in an AssetManager so MeshRenderer handles keep them
    // alive.
    engine::asset::AssetManager assets_;
    engine::scene::Scene        scene_;
    WorldRuntime                world_;
    FlyCamera                   camera_;
    std::optional<RemoteBridge> remote_;

    std::uint64_t presented_ = 0;
    double        frame_time_sum_ = 0.0; // avg-frame-time report at exit
    std::uint64_t frame_time_samples_ = 0;
    std::uint64_t last_ticks_ = 0;
};

} // namespace quat::app
