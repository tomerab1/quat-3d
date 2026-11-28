#include "app/application.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "app/asset_root.hpp"
#include "app/demo_scenes.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/scene/bounds.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/view_clip.hpp"

namespace quat::app {
namespace {

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

    platform_.window =
        SDL_CreateWindow(k_window_title, config_.window_width, config_.window_height,
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

    // The window surface is created between the instance and the physical
    // device choice, so only a GPU that can present to this window is picked.
    SDL_Window* window = platform_.window;
    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = k_window_title,
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
    shader_reload_ = engine::rhi::ShaderHotReload::create_from_environment( // R2
        pipeline_cache_, project_root() / "assets" / "shaders", cooked_shader_dir());
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
    const bool scene_ok = world_.build_scene(scene_, assets_, allocator_, setup_transfer,
                                             project_root());

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
    // Atmosphere LUTs + dynamic IBL probe, lit by the scene's sun.
    renderer_.create_environment(scene_, pipeline_cache_, shader_dir_);
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
                  .remote = &*remote_});
#endif
    return true;
}

void Application::init_scripts() {
    input_.init();
    engine::script::ScriptServices services;
    services.input = &input_.state();
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
    remote_targets.cam_near_override = &camera_.near_override;
    remote_targets.cam_far_override = &camera_.far_override;
    remote_targets.cam_derived_clip = &camera_.derived_clip();
    remote_targets.render_settings = &renderer_.render_settings();
    remote_targets.material_overrides = &renderer_.material_overrides();
    remote_targets.reset_temporal = [this] { renderer_.reset_temporal(); };
    world_.fill_remote_targets(remote_targets);
    remote_targets.play = world_.has_demo_physics() ? nullptr : play_mode_.request();
#ifdef ENGINE_EDITOR
    editor_.fill_remote_targets(remote_targets);
#endif
    remote_targets.shader_reload = shader_reload_.get(); // A6: shader.create / update compile
    remote_targets.project_root = project_root();
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
#ifdef ENGINE_EDITOR
    keyboard_blocked = editor_.keyboard_captured();
#endif
    input_.sample(keyboard_blocked);
    return running;
}

bool Application::run_frame() {
    // Remote-control requests (and primitive resolution) run here: after
    // input, before play-mode transitions, scene file requests and the
    // scene tick.
    remote_->poll();
#ifdef ENGINE_EDITOR
    editor_.begin_frame(); // UI frame + Play/Stop, save/load, instantiate
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
    const std::optional<engine::remote::CameraPose> shot = remote_->screenshot_view();
    const engine::scene::CameraMatrices view = shot ? shot_view(*shot, aspect) : cam;
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
    if (view_camera != view_camera_) {
        view_camera_ = view_camera;
        renderer_.reset_temporal();
    }
    return cam;
}

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
