#include "app/application.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "app/asset_root.hpp"
#include "app/demo_scenes.hpp"
#include "app/window_icon.hpp"
#include "engine/core/profile.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/scene/bounds.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/view_clip.hpp"

namespace quat::app {
namespace {

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

// The user's input rebinding file (G5): QUAT_INPUT_USER_FILE (empty: none),
// else input.json in SDL's per-user preferences directory.
std::optional<std::filesystem::path> user_input_file() {
    if (const char* env = std::getenv("QUAT_INPUT_USER_FILE"); env != nullptr) {
        if (*env == '\0') return std::nullopt;
        return engine::core::path_from_utf8(env);
    }
    char* pref = SDL_GetPrefPath("quat-3d", "game");
    if (pref == nullptr) return std::nullopt;
    std::filesystem::path out = engine::core::path_from_utf8(pref) / "input.json";
    SDL_free(pref);
    return out;
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
    // the swapchain explicitly; the device owns the surface and destroys it
    // after the swapchain and before the instance.
    if (device_.handle() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_.handle());
        world_.shutdown(); // waits for a running terrain job, frees its upload pool
        renderer_.destroy_frame_sync();
        swapchain_ = engine::rhi::Swapchain{}; // destroy swapchain before surface
    }
}

int Application::run() {
    // R4: the frame profiler records the main thread's zones; each loop
    // iteration is one profiler frame.
    QUAT_THREAD_NAME("main");
    engine::core::profiler().bind_current_thread();
    if (!init_platform() || !init_subsystems()) return 1;
    while (pump_events() && run_frame()) {
        QUAT_FRAME_MARK();
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

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;
    if (config_.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    platform_.window = SDL_CreateWindow(config_.window_title.c_str(), config_.window_width,
                                        config_.window_height, flags);
    if (platform_.window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    if (config_.window_icon) {
        if (auto icon = set_window_icon(platform_.window, *config_.window_icon); !icon) {
            std::fprintf(stderr, "[warn] %s\n", icon.error().message.c_str());
        }
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (sdl_extensions == nullptr) {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    // The window surface is created between the instance and the physical
    // device choice, so only a GPU that can present to this window is picked.
    SDL_Window* window = platform_.window;
    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = config_.window_title.c_str(),
        .create_surface = [window](VkInstance instance)
            -> std::expected<VkSurfaceKHR, engine::core::Error> {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
                return std::unexpected(engine::core::Error{
                    std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError()});
            }
            return surface;
        },
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

    // QUAT_NO_MUTABLE_SWAPCHAIN=1: behave as a driver without
    // VK_KHR_swapchain_mutable_format, so the editor UI takes its
    // copy-composite fallback (checking it on a machine that has the extension).
    if (std::getenv("QUAT_NO_MUTABLE_SWAPCHAIN") != nullptr) {
        device_.simulate_missing_swapchain_mutable_format(true);
    }
    auto swapchain_result = engine::rhi::Swapchain::create(
        device_, device_.surface(), window_pixel_extent(platform_.window));
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
    // Pipeline-cache paths are UTF-8 strings (opened via path_from_utf8).
    shader_dir_ = engine::core::path_to_utf8(cooked_shader_dir());
    if (config_.shader_hot_reload) { // R2; never in the game player
        shader_reload_ = engine::rhi::ShaderHotReload::create_from_environment(
            pipeline_cache_, data_root() / "assets" / "shaders", cooked_shader_dir());
    }
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
                                                      device_.max_sampler_anisotropy(),
                                                      device_.texture_caps()};

    bool with_ui = false;
#ifdef ENGINE_EDITOR
    // Editor shell (ImGui context + SDL3 input backend). Created before the
    // frame slots: each slot's ImGuiPass uploads the font atlas of this
    // context. QUAT_NO_UI=1 disables it (deterministic headless renders).
    editor_.create(platform_.window, config_.editor_ui);
    with_ui = editor_.active();
#endif

    // Per-frame-in-flight deferred chains + shared temporal state.
    const bool frames_ok = renderer_.create_passes(device_, allocator_, pipeline_cache_,
                                                   setup_transfer, shader_dir_, swapchain_,
                                                   with_ui);

    // QUAT_GLTF=<path> instantiates a real glTF scene graph; otherwise a
    // spinning demo cube stands in (see demo_scenes.hpp).
    // A game's start scene (quat-player) replaces the demo scenes and
    // brings its own look (render settings).
    const bool scene_ok =
        world_.build_scene(scene_, assets_, allocator_, setup_transfer, project_root(),
                           renderer_.render_settings(), config_.start_scene);

    vkDestroyCommandPool(vk_device, setup_pool, nullptr);

    if (!frames_ok || !scene_ok) {
        std::fprintf(stderr, "[fatal] %s: %s\n", "deferred renderer setup failed",
                     "see [fatal] lines above");
        return false;
    }

    if (!world_.from_scene_file()) {
        add_default_lighting(scene_, !world_.from_gltf(), world_.demo_root());
    }
    // QUAT_TIME: a world clock places the sun before the sky is first baked.
    add_env_world_clock(scene_);
    add_env_season(scene_); // QUAT_SEASON: the clock's date and the weather (R25.1)
    // Atmosphere LUTs + dynamic IBL probe, lit by the scene's sun.
    renderer_.create_environment(scene_, pipeline_cache_, shader_dir_);
    renderer_.create_reflections(pipeline_cache_, shader_dir_); // R21: HDRI + SSR
    load_startup_hdri();
    // Terrain, networking (QUAT_HOST / QUAT_JOIN) and the QUAT_TERRAIN tile.
    world_.init(device_, allocator_, pipeline_cache_, shader_dir_, project_root(), scene_);

    // Free-fly view camera, seeded by framing the scene.
    // Issue #8: frame the demo content (not its ground plate) for the view it
    // renders into. The editor's Viewport panel is narrower than the window,
    // so its framing assumes at most 4:3.
    {
        const VkExtent2D extent = renderer_.present_extent();
        float aspect = extent.height == 0 ? 16.0F / 9.0F
                                          : static_cast<float>(extent.width) /
                                                static_cast<float>(extent.height);
        if (with_ui) aspect = std::min(aspect, 4.0F / 3.0F);
        camera_ = FlyCamera::create(scene_, {.physics_mode = world_.physics_mode(),
                                             .aspect = aspect,
                                             .exclude = world_.framing_exclude()});
    }

    // ---- Per-frame command + sync resources --------------------------------
    renderer_.create_frame_sync();
    renderer_.set_screenshot_path(config_.screenshot_path);

    frame_timer_.skip(SDL_GetTicksNS()); // setup time is not frame time
    init_scripts();
    connect_remote();
    world_.init_audio(project_root());
#ifdef ENGINE_EDITOR
    editor_.bind({.device = &device_,
                  .allocator = &allocator_,
                  .assets = &assets_,
                  .scene = &scene_,
                  .world = &world_,
                  .camera = &camera_,
                  .play_mode = &play_mode_,
                  .shader_reload = shader_reload_.get(),
                  .render_settings = &renderer_.render_settings(),
                  .reset_temporal = [this] { renderer_.reset_temporal(); },
                  .hdri_info = [this] { return hdri_info(); },
                  .set_hdri = [this](const std::string& relative) {
                      return renderer_.set_hdri(relative, project_root());
                  },
                  .remote = &*remote_,
                  .game = config_.game ? &*config_.game : nullptr,
                  .build_job = &build_job_});
#endif
    return true;
}

void Application::init_scripts() {
    input_.init();
    input_runtime_.init(project_root(), user_input_file());
    engine::script::ScriptServices services;
    services.input = &input_.state();
    services.actions = &input_runtime_.actions();
    services.spawn_model = [this](const std::filesystem::path& absolute) {
        return remote_->spawn_model(absolute);
    };
    world_.init_scripts(project_root(), std::move(services));
    // Start in Play (the transition runs on the first frame) for QUAT_PLAY=1
    // and, without the editor, for a QUAT_SCENE game. The showcase, glTF and
    // screenshot runs stay static; the physics / character demos own their
    // world and have no play mode.
    bool play_on_start = std::getenv("QUAT_PLAY") != nullptr;
#ifndef ENGINE_EDITOR
    play_on_start = play_on_start || world_.from_scene_file();
#endif
    if (play_on_start && !world_.has_demo_physics()) *play_mode_.request() = true;
}

#if !defined(QUAT_SHIPPING)
engine::remote::HdriInfo Application::hdri_info() const {
    const HdriStatus& s = renderer_.hdri_status();
    return engine::remote::HdriInfo{s.path, s.loaded, s.width, s.height, s.sun_found, s.error};
}
#endif // !QUAT_SHIPPING

void Application::load_startup_hdri() {
    std::string hdri = world_.startup_environment().hdri;
    if (const char* env = std::getenv("QUAT_HDRI"); env != nullptr && *env != '\0') {
        hdri = env;
        renderer_.render_settings().environment.source =
            static_cast<int>(engine::renderer::SkySource::hdri);
    }
    if (!hdri.empty()) (void)renderer_.set_hdri(hdri, project_root()); // failures are logged
}

void Application::connect_remote() {
    // Remote control (ROADMAP A2/A3): QUAT_REMOTE=<port> opens the loopback
    // JSON-RPC endpoint the quat-mcp bridge talks to (always token-protected:
    // QUAT_REMOTE_TOKEN or a generated one in a user-only token file). The
    // bridge also resolves Primitive components
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
    remote_targets.cam_lens = &camera_.lens;
    remote_targets.cam_near_override = &camera_.near_override;
    remote_targets.cam_far_override = &camera_.far_override;
    remote_targets.cam_derived_clip = &camera_.derived_clip();
    remote_targets.render_settings = &renderer_.render_settings();
    remote_targets.wind_clock = [this] { return renderer_.wind_clock(); };
    remote_targets.material_overrides = &renderer_.material_overrides();
    remote_targets.reset_temporal = [this] { renderer_.reset_temporal(); };
    remote_targets.input = &input_runtime_;
    remote_targets.view_aspect = [this] {
        VkExtent2D e = renderer_.present_extent();
#ifdef ENGINE_EDITOR
        e = editor_.scene_extent(e);
#endif
        return e.height == 0 ? 0.0F : static_cast<float>(e.width) / static_cast<float>(e.height);
    };
    world_.fill_remote_targets(remote_targets);
#if !defined(QUAT_SHIPPING)
    // environment.* HDRI control (R21), remote control only.
    remote_targets.hdri_info = [this] { return hdri_info(); };
    remote_targets.set_hdri = [this](const std::string& relative) {
        return renderer_.set_hdri(relative, project_root());
    };
    // terrain.status only (remote control): wired here, not in WorldRuntime,
    // whose library the shipping player shares with quat-player.
    remote_targets.terrain_status = [this] { return world_.terrain_status(); };
#endif
    remote_targets.play = world_.has_demo_physics() ? nullptr : play_mode_.request();
#ifdef ENGINE_EDITOR
    editor_.fill_remote_targets(remote_targets);
#endif
    remote_targets.shader_reload = shader_reload_.get(); // A6: shader.create / update compile
    remote_targets.project_root = project_root();
    // Game projects (P4): project.info / project.build; shipping packages
    // keep the endpoint closed whatever QUAT_REMOTE says.
    remote_targets.allow_endpoint = config_.remote_allowed;
    remote_targets.game = config_.game ? &*config_.game : nullptr;
    remote_targets.build_job = &build_job_;
    remote_.emplace(std::move(remote_targets), k_frames_in_flight);
}

bool Application::pump_events() {
    camera_.begin_input(); // per-frame accumulated mouse-look delta
    input_.begin_frame();
    bool running = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef ENGINE_EDITOR
        editor_.process_event(event);
#endif
        input_.process_event(event);
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
    bool keyboard_blocked = false;
    bool pointer_blocked = false;
#ifdef ENGINE_EDITOR
    keyboard_blocked = editor_.keyboard_captured();
    pointer_blocked = !editor_.camera_accepts_clicks(); // over a panel, not the viewport
#endif
    input_.sample(platform_.window, keyboard_blocked, pointer_blocked);
#ifdef ENGINE_EDITOR
    // Pointer positions and edge zones are measured on the Viewport panel.
    if (const auto rect = editor_.viewport_rect(); editor_.active() && rect[2] > rect[0]) {
        input_.set_viewport({rect[0], rect[1]}, {rect[2] - rect[0], rect[3] - rect[1]});
    }
#endif
    return running;
}

bool Application::run_frame() {
    // Remote-control requests (and primitive resolution) run here: after
    // input, before play-mode transitions, scene file requests and the
    // scene tick.
    remote_->poll();
#ifdef ENGINE_EDITOR
    {
        QUAT_ZONE_N("editor_begin");
        editor_.begin_frame(); // UI frame + Play/Stop, save/load, instantiate
    }
    if (editor_.take_font_atlas_changed()) renderer_.reload_ui_fonts(); // DPI change
#else
    // Play/Stop requested by sim.play / sim.stop; a transition swaps worlds,
    // so no temporal history carries across.
    if (world_.apply_play(play_mode_, scene_)) renderer_.reset_temporal();
#endif

    const VkExtent2D pixel_extent = window_pixel_extent(platform_.window);
    if (pixel_extent.width == 0 || pixel_extent.height == 0) {
        SDL_Delay(10); // minimized — nothing to draw
        // Time spent minimised is not frame time (#61): without this the first
        // frame after restore would advance everything by the whole gap.
        frame_timer_.skip(SDL_GetTicksNS());
        run_unrendered_step();
        return true;
    }
    if (!renderer_.ensure_swapchain(pixel_extent)) return false;

    const std::uint32_t slot = renderer_.wait_for_slot();
    if (shader_reload_) shader_reload_->update(presented_); // R2: swap rebuilt pipelines
    remote_->on_frame_complete(slot); // the slot's screenshot readback landed
    switch (renderer_.acquire_and_begin()) {
    case FrameRenderer::Acquire::retry: frame_timer_.skip(SDL_GetTicksNS()); return true;
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

    const float dt = frame_timer_.tick(SDL_GetTicksNS());
    renderer_.advance_time(dt);
    const float sim_dt = remote_->advance_sim(dt); // A5: pause / step / time scale

    const engine::scene::CameraMatrices cam = update_world(dt, sim_dt, aspect);
    // A screenshot with its own camera renders this frame from there (issue
    // #41); terrain streaming, audio and the editor UI stay with the user's view.
#if defined(QUAT_SHIPPING)
    const engine::scene::CameraMatrices& view = cam; // no remote screenshots
#else
    const std::optional<engine::remote::CameraPose> shot = remote_->screenshot_view();
    const engine::scene::CameraMatrices view = shot ? shot_view(*shot, aspect) : cam;
#endif
    // A camera cut (a remote teleport, or a screenshot's own camera and the
    // frame returning from it): the temporal history restarts instead of
    // blending across the jump (R11).
    if (remote_->take_camera_cut()) renderer_.reset_temporal();
    const FrameScene frame_scene = renderer_.gather(scene_, view, draw_extent, presented_);

    world_.update_terrain(scene_, cam.position);
    camera_.pos.y += world_.take_demo_shift(); // the content moved onto the terrain (#7)
#ifdef ENGINE_EDITOR
    editor_.handle_ground_click();
#endif
    world_.service_navmesh_request(scene_);

    FrameInputs frame_inputs;
    frame_inputs.scene = &frame_scene;
    frame_inputs.registry = &scene_.registry();
    frame_inputs.cam = &view;
    frame_inputs.draw_extent = draw_extent;
    frame_inputs.dt = dt;
#ifdef ENGINE_EDITOR
    // The editor views through its fly camera unless Play uses the scene's.
    if (!editor_.through_scene_camera()) frame_inputs.view_lens = &camera_.lens;
#endif
    frame_inputs.grass = world_.grass();
    frame_inputs.particles = world_.particles();
    frame_inputs.sim_dt = sim_dt;
    if (world_.terrain_drawable()) {
        frame_inputs.terrain = &world_.terrain_pass();
        frame_inputs.terrain_anchor = world_.terrain_anchor();
        frame_inputs.terrain_snowline = world_.terrain_snowline();
    }
#ifdef ENGINE_EDITOR
    frame_inputs.build_ui = [&]() {
        QUAT_ZONE_N("editor_ui");
        // record() calls this after batching, so the count is this frame's.
        return editor_.build_ui(dt, static_cast<int>(renderer_.draw_count()), cam);
    };
#endif
    if (!renderer_.record(frame_inputs)) return false;
    // A5 debug.stats: the frame's submitted draws, counted after batching.
    remote_->note_draws(renderer_.draw_count());

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

void Application::run_unrendered_step() {
    // Only a queued debug.step advances the clock here, so real-time
    // simulation stays frozen while minimised but a stepping remote caller
    // still gets its reply. No camera input or audio: nothing is shown.
    const float sim_dt = remote_->advance_sim_unrendered();
    if (sim_dt <= 0.0F) return;
    const std::optional<PlayStep> play = play_step();
    world_.simulate(scene_, presented_, 0.0F, sim_dt, play ? &*play : nullptr);
}

std::optional<PlayStep> Application::play_step() const {
#ifdef ENGINE_EDITOR
    return editor_.play_step(camera_.pos);
#else
    if (!play_mode_.active()) return std::nullopt;
    return PlayStep{.use_scene_camera = true, .fly_camera_pos = camera_.pos};
#endif
}

engine::scene::CameraMatrices Application::update_world(float dt, float sim_dt, float aspect) {
    QUAT_ZONE_N("update_world");
    const bool character_mode = world_.character_mode();
    input_runtime_.update(input_.state(), dt, view_yaw_); // G5: once per frame

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

    // Camera rigs (G15.1) before the scene tick resolves their transforms.
    // They take input only while the game view is live, so the editor's fly
    // camera and a rig never share WASD.
#ifdef ENGINE_EDITOR
    const bool rig_input = editor_.playing() && editor_.through_scene_camera();
#else
    const bool rig_input = true;
#endif
    input_runtime_.update_rigs(scene_, world_.ground_height(), world_.simulated_physics(), rig_input, dt);

    const std::optional<PlayStep> play = play_step();
    world_.simulate(scene_, presented_, dt, sim_dt, play ? &*play : nullptr);

    // Third-person follow camera, placed after the character has moved.
    if (character_mode) camera_.follow(scene_, world_.character_entity());
    update_view_clip();

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
    // Switching the view to another camera is a cut: TAA / velocity must not
    // blend or reproject across it (R11).
#ifdef ENGINE_EDITOR
    const entt::entity view_camera = editor_.through_scene_camera()
                                         ? engine::scene::find_active_camera(scene_.registry())
                                         : entt::entity{entt::null};
#else
    const entt::entity view_camera = engine::scene::find_active_camera(scene_.registry());
#endif
    // The view's heading, for next frame's camera_relative actions.
    const glm::vec3 view_forward = -glm::vec3(glm::inverse(cam.view)[2]);
    if (glm::length(glm::vec2(view_forward.x, view_forward.z)) > 1e-4F) {
        view_yaw_ = std::atan2(view_forward.z, view_forward.x);
    }
    if (view_camera != view_camera_) {
        view_camera_ = view_camera;
        renderer_.reset_temporal();
    }
    return cam;
}

#if !defined(QUAT_SHIPPING)
engine::scene::CameraMatrices Application::shot_view(const engine::remote::CameraPose& pose,
                                                     float aspect) const {
    // Clip planes derived for the shot's own position, with its overrides.
    const engine::scene::ViewExtent extent{.drawables = scene_bounds_,
                                           .terrain = world_.terrain_view_bounds(pose.position)};
    const engine::scene::ClipRange clip = engine::scene::apply_clip_overrides(
        engine::scene::derive_clip_range(pose.position, extent), pose.near_override,
        pose.far_override);
    return view_matrices(pose.position, glm::radians(pose.yaw_deg),
                         glm::radians(glm::clamp(pose.pitch_deg, -89.0F, 89.0F)),
                         glm::radians(pose.fov_deg), clip, aspect);
}
#endif // !QUAT_SHIPPING

void Application::update_view_clip() {
    // Scanning every drawable is cheap but not free: refresh the bounds a few
    // times a second (the far plane keeps a margin for what moved meanwhile).
    if (!scene_bounds_valid_ || presented_ - scene_bounds_frame_ >= k_bounds_refresh_frames) {
        scene_bounds_ = engine::scene::scene_draw_bounds(scene_.registry());
        scene_bounds_frame_ = presented_;
        scene_bounds_valid_ = true;
    }
    const engine::scene::ViewExtent extent{.drawables = scene_bounds_,
                                           .terrain = world_.terrain_view_bounds(camera_.pos)};
    camera_.set_derived_clip(scene_, engine::scene::derive_clip_range(camera_.pos, extent));
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
