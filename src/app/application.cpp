#include "app/application.hpp"

#include <cstdio>
#include <utility>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "app/demo_scenes.hpp"
#include "engine/scene/components.hpp"

namespace quat::app {
namespace {

constexpr int  k_window_width  = 1280;
constexpr int  k_window_height = 720;
constexpr char k_window_title[] = "quat-3d";

#ifndef NDEBUG
constexpr bool k_enable_validation = true;
#else
constexpr bool k_enable_validation = false;
#endif

VkExtent2D window_pixel_extent(SDL_Window* window) {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return VkExtent2D{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

} // namespace

Application::Platform::~Platform() {
    if (window != nullptr) SDL_DestroyWindow(window);
    if (sdl_initialized) SDL_Quit();
}

Application::Application(AppConfig config) : config_(std::move(config)) {}

Application::~Application() {
    // ---- Teardown ----------------------------------------------------------
    // GPU objects are freed by the members' destructors (reverse declaration
    // order: subsystems, then the swapchain, allocator and device, then the
    // window and SDL). Wait for the GPU first and release the frame sync and
    // the surface explicitly: the surface must go after the swapchain and
    // before the instance.
    if (device_.handle() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_.handle());
        world_.shutdown(); // waits for a running terrain job, frees its upload pool
        renderer_.destroy_frame_sync();
        swapchain_ = engine::rhi::Swapchain{}; // destroy swapchain before surface
        if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(device_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

int Application::run() {
    if (!init_platform() || !init_subsystems()) return 1;
    while (pump_events() && run_frame()) {
    }
    report_stats();
    return 0;
}

bool Application::init_platform() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    platform_.sdl_initialized = true;

    platform_.window = SDL_CreateWindow(k_window_title, k_window_width, k_window_height,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (platform_.window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (sdl_extensions == nullptr) {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = k_window_title,
    });
    if (!device_result) {
        std::fprintf(stderr, "[fatal] device creation failed: %s\n",
                     device_result.error().message.c_str());
        return false;
    }
    device_ = std::move(*device_result);

    auto allocator_result = engine::rhi::GpuAllocator::create(device_);
    if (!allocator_result) {
        std::fprintf(stderr, "[fatal] allocator creation failed: %s\n",
                     allocator_result.error().message.c_str());
        return false;
    }
    allocator_ = std::move(*allocator_result);

    if (!SDL_Vulkan_CreateSurface(platform_.window, device_.instance(), nullptr, &surface_)) {
        std::fprintf(stderr, "[fatal] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }

    auto swapchain_result =
        engine::rhi::Swapchain::create(device_, surface_, window_pixel_extent(platform_.window));
    if (!swapchain_result) {
        std::fprintf(stderr, "[fatal] swapchain creation failed: %s\n",
                     swapchain_result.error().message.c_str());
        return false;
    }
    swapchain_ = std::move(*swapchain_result);
    return true;
}

bool Application::init_subsystems() {
    // ---- Deferred renderer + demo scene ------------------------------------
    auto pipeline_cache = engine::rhi::PipelineCache::create(device_);
    if (!pipeline_cache) {
        std::fprintf(stderr, "[fatal] %s: %s\n", "pipeline cache creation failed",
                     pipeline_cache.error().message.c_str());
        return false;
    }
    pipeline_cache_ = std::move(*pipeline_cache);
    shader_dir_ = QUAT_COOKED_SHADER_DIR;
    const VkDevice vk_device = device_.handle();

    // One-time transfer context for setup uploads (demo mesh/material + each
    // pass's fallback resources). Destroyed once setup completes.
    VkCommandPool setup_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // Graphics family: mip-generation blits (vkCmdBlitImage) are only legal
        // on a graphics-capable queue.
        info.queueFamilyIndex = device_.queue_families().graphics;
        vkCreateCommandPool(vk_device, &info, nullptr, &setup_pool);
    }
    const engine::rhi::TransferContext setup_transfer{vk_device, setup_pool,
                                                      device_.graphics_queue(),
                                                      device_.max_sampler_anisotropy()};

    bool with_ui = false;
#ifdef ENGINE_EDITOR
    // Editor shell (ImGui context + SDL3 input backend). Created before the
    // frame slots: each slot's ImGuiPass uploads the font atlas of this
    // context. QUAT_NO_UI=1 disables it (deterministic headless renders).
    editor_.create(platform_.window, config_.editor_ui);
    renderer_.set_render_settings(&editor_.render_settings());
    with_ui = editor_.active();
#endif

    // Per-frame-in-flight deferred chains + shared temporal state.
    const bool frames_ok = renderer_.create_passes(device_, allocator_, pipeline_cache_,
                                                   setup_transfer, shader_dir_, swapchain_,
                                                   with_ui);

    // QUAT_GLTF=<path> instantiates a real glTF scene graph; otherwise a
    // spinning demo cube stands in (see demo_scenes.hpp).
    const bool scene_ok = world_.build_scene(scene_, assets_, allocator_, setup_transfer);

    vkDestroyCommandPool(vk_device, setup_pool, nullptr);

    if (!frames_ok || !scene_ok) {
        std::fprintf(stderr, "[fatal] %s: %s\n", "deferred renderer setup failed",
                     "see [fatal] lines above");
        return false;
    }

    add_default_lighting(scene_, !world_.from_gltf());
    // Atmosphere LUTs + dynamic IBL probe, lit by the scene's sun.
    renderer_.create_environment(scene_, pipeline_cache_, shader_dir_);
    // Terrain, networking (QUAT_HOST / QUAT_JOIN) and the QUAT_TERRAIN tile.
    world_.init(device_, allocator_, pipeline_cache_, shader_dir_, scene_);

    // Free-fly view camera, seeded by framing the scene.
    camera_ = FlyCamera::create(scene_, world_.physics_mode());

    // ---- Per-frame command + sync resources --------------------------------
    renderer_.create_frame_sync();
    renderer_.set_screenshot_path(config_.screenshot_path);

    last_ticks_ = SDL_GetTicksNS();
    connect_remote();
    world_.init_audio(QUAT_PROJECT_ROOT);
#ifdef ENGINE_EDITOR
    editor_.bind({.device = &device_,
                  .allocator = &allocator_,
                  .assets = &assets_,
                  .scene = &scene_,
                  .world = &world_,
                  .camera = &camera_});
#endif
    return true;
}

void Application::connect_remote() {
    // Remote control (ROADMAP A2/A3): QUAT_REMOTE=<port> opens the loopback
    // JSON-RPC endpoint the quat-mcp bridge talks to (QUAT_REMOTE_TOKEN
    // requires session.hello). The bridge also resolves Primitive components
    // every frame, endpoint or not. See src/app/remote_bridge.hpp.
    RemoteBridgeTargets remote_targets;
    remote_targets.device = &device_;
    remote_targets.allocator = &allocator_;
    remote_targets.assets = &assets_;
    remote_targets.scene = &scene_;
    remote_targets.frame_counter = &presented_;
    remote_targets.cam_pos = &camera_.pos;
    remote_targets.cam_yaw = &camera_.yaw;
    remote_targets.cam_pitch = &camera_.pitch;
    remote_targets.cam_fov_y = &camera_.fov_y;
    world_.fill_remote_targets(remote_targets);
#ifdef ENGINE_EDITOR
    editor_.fill_remote_targets(remote_targets, world_.has_demo_physics());
#endif
    remote_targets.project_root = QUAT_PROJECT_ROOT;
    remote_.emplace(std::move(remote_targets), k_frames_in_flight);
}

bool Application::pump_events() {
    camera_.begin_input(); // per-frame accumulated mouse-look delta
    bool running = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef ENGINE_EDITOR
        editor_.process_event(event);
#endif
        switch (event.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_ESCAPE) running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            renderer_.request_swapchain_recreate();
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION: {
            bool accept_clicks = true;
#ifdef ENGINE_EDITOR
            accept_clicks = editor_.camera_accepts_clicks();
#endif
            camera_.handle_event(event, platform_.window, accept_clicks);
            break;
        }
        default:
            break;
        }
    }
    return running;
}

bool Application::run_frame() {
    // Remote-control requests (and primitive resolution) run here: after
    // input, before play-mode transitions, scene file requests and the
    // scene tick.
    remote_->poll();
#ifdef ENGINE_EDITOR
    editor_.begin_frame(); // UI frame + Play/Stop, save/load, instantiate
#endif

    const VkExtent2D pixel_extent = window_pixel_extent(platform_.window);
    if (pixel_extent.width == 0 || pixel_extent.height == 0) {
        SDL_Delay(10); // minimized — nothing to draw
        return true;
    }
    if (!renderer_.ensure_swapchain(pixel_extent)) return false;

    const std::uint32_t slot = renderer_.wait_for_slot();
    remote_->on_frame_complete(slot); // the slot's screenshot readback landed
    switch (renderer_.acquire_and_begin()) {
    case FrameRenderer::Acquire::retry: return true;
    case FrameRenderer::Acquire::failed: return false;
    case FrameRenderer::Acquire::ok: break;
    }

    // The scene renders at `draw_extent`; the swapchain presents at
    // `present_extent`. They differ only in editor mode, where the scene
    // fills the Viewport panel (sized by last frame's UI) instead of the
    // whole window.
    VkExtent2D draw_extent = renderer_.present_extent();
#ifdef ENGINE_EDITOR
    draw_extent = editor_.scene_extent(draw_extent);
#endif
    const float aspect =
        draw_extent.height == 0
            ? 1.0F
            : static_cast<float>(draw_extent.width) / static_cast<float>(draw_extent.height);

    const std::uint64_t now_ticks = SDL_GetTicksNS();
    const float dt = static_cast<float>(now_ticks - last_ticks_) * 1e-9F;
    last_ticks_ = now_ticks;
    renderer_.advance_time(dt);

    const engine::scene::CameraMatrices cam = update_world(dt, aspect);
    const FrameScene frame_scene = renderer_.gather(scene_, cam, draw_extent, presented_);

    world_.update_terrain(scene_, cam.position);
#ifdef ENGINE_EDITOR
    editor_.handle_ground_click();
#endif
    world_.service_navmesh_request(scene_);

    FrameInputs frame_inputs;
    frame_inputs.scene = &frame_scene;
    frame_inputs.cam = &cam;
    frame_inputs.draw_extent = draw_extent;
    frame_inputs.dt = dt;
    if (world_.terrain_drawable()) {
        frame_inputs.terrain = &world_.terrain_pass();
        frame_inputs.terrain_anchor = world_.terrain_anchor();
        frame_inputs.terrain_snowline = world_.terrain_snowline();
    }
#ifdef ENGINE_EDITOR
    frame_inputs.build_ui = [&]() {
        return editor_.build_ui(
            dt, static_cast<int>(frame_scene.draws.size() + frame_scene.transparent_draws.size()),
            cam);
    };
#endif
    if (!renderer_.record(frame_inputs)) return false;

    remote_->record_capture(renderer_.command_buffer(), renderer_.frame_slot(),
                            renderer_.capture_source());

    const std::uint64_t max_frames = config_.max_frames;
    if (!renderer_.submit_and_present(max_frames != 0 && presented_ + 1 >= max_frames)) {
        return false;
    }

    // Average frame time over the run, skipping the first frame (it carries
    // one-time pipeline/descriptor warmup). Reported at exit.
    if (presented_ > 0) {
        frame_time_sum_ += dt;
        frame_time_samples_ += 1;
    }
    // Advances every frame (audit A1): drives TAA jitter, the demo cube,
    // frame stats and engine.info — not only QUAT_MAX_FRAMES runs.
    ++presented_;
    return max_frames == 0 || presented_ < max_frames;
}

engine::scene::CameraMatrices Application::update_world(float dt, float aspect) {
    const bool character_mode = world_.character_mode();

    // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift = fast.
    camera_.apply_look();
    FlyInputGate gate;
#ifdef ENGINE_EDITOR
    gate = editor_.input_gate(camera_.looking(), character_mode);
#endif
    if (character_mode) {
        world_.drive_character(scene_, camera_.character_wish(gate), dt);
    } else {
        camera_.fly(scene_, gate, dt);
    }

    std::optional<PlayStep> play;
#ifdef ENGINE_EDITOR
    play = editor_.play_step(camera_.pos);
#endif
    world_.simulate(scene_, presented_, dt, play ? &*play : nullptr);

    // Third-person follow camera, placed after the character has moved.
    if (character_mode) camera_.follow(scene_, world_.character_entity());

#ifdef ENGINE_EDITOR
    // The editor's fly camera is the default view in BOTH modes — Play
    // must never take movement away from the user (old scene files also
    // carry stale "camera" entities that would hijack the view). The
    // menu-bar "Game camera" checkbox opts the play view into the scene's
    // active Camera entity (a camera parented to a moving object follows
    // it); it falls back to the fly camera when none is active.
    const engine::scene::CameraMatrices cam =
        editor_.through_scene_camera() ? engine::scene::camera_system(scene_.registry(), aspect)
                                       : camera_.matrices(aspect);
    world_.update_audio(scene_, cam.view, editor_.playing(), dt);
#else
    const engine::scene::CameraMatrices cam =
        engine::scene::camera_system(scene_.registry(), aspect);
    world_.update_audio(scene_, cam.view, /*live=*/true, dt);
#endif
    return cam;
}

void Application::report_stats() const {
    if (frame_time_samples_ > 0) {
        const double avg_ms =
            frame_time_sum_ / static_cast<double>(frame_time_samples_) * 1000.0;
        std::fprintf(stderr, "[stats] avg frame %.2f ms (%.1f fps) over %llu frames\n", avg_ms,
                     avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0,
                     static_cast<unsigned long long>(frame_time_samples_));
    }
}

} // namespace quat::app
