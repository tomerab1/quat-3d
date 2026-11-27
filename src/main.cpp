// quat-3d engine entry point.
//
// Phase 2, Slice 2.5: a hard-coded triangle is drawn via the render graph. Each
// frame builds a RenderGraph that imports the acquired swapchain image and runs
// a single GraphicsPass (dynamic rendering) that clears to black and draws the
// triangle from mesh.slang. The graph inserts the layout barriers and the
// trailing transition to PRESENT_SRC. Two frames in flight.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "app/audio_runtime.hpp"
#include "app/demo_scenes.hpp"
#include "app/fly_camera.hpp"
#include "app/frame_renderer.hpp"
#include "app/nav_bake.hpp"
#include "app/remote_bridge.hpp"
#include "app/world_runtime.hpp"
#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/dynamic_ibl.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/net/session.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/terrain/streamer.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/renderer/transparent_pass.hpp"

#ifdef ENGINE_EDITOR
#include "editor/scene_snapshot.hpp"
#include "engine/editor/editor.hpp"
#include "engine/renderer/imgui_pass.hpp"
#endif
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/rhi/swapchain.hpp"

namespace {

constexpr int  k_window_width  = 1280;
constexpr int  k_window_height = 720;
constexpr char k_window_title[] = "quat-3d";
using quat::app::k_frames_in_flight;

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

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        k_window_title, k_window_width, k_window_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (sdl_extensions == nullptr) {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = k_window_title,
    });
    if (!device_result) {
        std::fprintf(stderr, "[fatal] device creation failed: %s\n",
                     device_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Device& device = *device_result;
    const VkDevice vk_device = device.handle();

    auto allocator_result = engine::rhi::GpuAllocator::create(device);
    if (!allocator_result) {
        std::fprintf(stderr, "[fatal] allocator creation failed: %s\n",
                     allocator_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::GpuAllocator& allocator = *allocator_result;


    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, device.instance(), nullptr, &surface)) {
        std::fprintf(stderr, "[fatal] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto swapchain_result =
        engine::rhi::Swapchain::create(device, surface, window_pixel_extent(window));
    if (!swapchain_result) {
        std::fprintf(stderr, "[fatal] swapchain creation failed: %s\n",
                     swapchain_result.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Swapchain& swapchain = *swapchain_result;

    // ---- Deferred renderer + demo scene ------------------------------------
    auto fatal = [&](const char* what, const std::string& detail) {
        std::fprintf(stderr, "[fatal] %s: %s\n", what, detail.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
    };

    auto pipeline_cache = engine::rhi::PipelineCache::create(device);
    if (!pipeline_cache) {
        fatal("pipeline cache creation failed", pipeline_cache.error().message);
        return 1;
    }

    const std::string shader_dir = QUAT_COOKED_SHADER_DIR;

    // One-time transfer context for setup uploads (demo mesh/material + each
    // pass's fallback resources). Destroyed once setup completes.
    VkCommandPool setup_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // Graphics family: mip-generation blits (vkCmdBlitImage) are only legal
        // on a graphics-capable queue.
        info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(vk_device, &info, nullptr, &setup_pool);
    }
    const engine::rhi::TransferContext setup_transfer{vk_device, setup_pool,
                                                      device.graphics_queue(),
                                                      device.max_sampler_anisotropy()};

#ifdef ENGINE_EDITOR
    // Editor shell (ImGui context + SDL3 input backend). Created before the
    // frame slots: each slot's ImGuiPass uploads the font atlas of this
    // context. QUAT_NO_UI=1 disables it (deterministic headless renders).
    engine::editor::EditorLayer editor;
    engine::editor::RendererSettings render_settings;
    std::string pending_instantiate; // set by the asset browser, consumed below
    std::string pending_save;        // scene file requests from the File menu
    std::string pending_load;
    // Play mode: snapshot taken on Play, restored on Stop; the play session
    // owns its own physics world (demo modes keep theirs).
    bool editor_play = false;
    bool play_use_scene_camera = false; // menu-bar "Game camera" checkbox
    bool editor_play_prev = false;
    std::optional<engine::editor::SceneSnapshot> play_snapshot;
    const bool editor_enabled = std::getenv("QUAT_NO_UI") == nullptr;
    if (editor_enabled) {
        if (auto e = engine::editor::EditorLayer::create(
                window, std::string(QUAT_PROJECT_ROOT) + "/assets/fonts/Roboto-Medium.ttf");
            e) {
            editor = std::move(*e);
        } else {
            std::fprintf(stderr, "[warn] editor disabled: %s\n", e.error().message.c_str());
        }
    }
#endif

    // Per-frame-in-flight deferred chains + temporal state (frame_renderer.hpp).
    quat::app::FrameRenderer renderer;
#ifdef ENGINE_EDITOR
    renderer.set_render_settings(&render_settings);
    const bool with_ui = editor.active();
#else
    const bool with_ui = false;
#endif
    const bool frames_ok = renderer.create_passes(device, allocator, *pipeline_cache,
                                                  setup_transfer, shader_dir, swapchain, with_ui);

    // Scene assets live in an AssetManager so MeshRenderer handles keep them
    // alive. QUAT_GLTF=<path> instantiates a real glTF scene graph; otherwise a
    // spinning demo cube stands in.
    engine::asset::AssetManager assets;
    engine::scene::Scene scene;
    quat::app::WorldRuntime world;
    const bool scene_ok = world.build_scene(scene, assets, allocator, setup_transfer);
    const bool physics_mode = world.physics_mode();
    const bool character_mode = world.character_mode();

    vkDestroyCommandPool(vk_device, setup_pool, nullptr);

    if (!frames_ok || !scene_ok) {
        fatal("deferred renderer setup failed", "see [fatal] lines above");
        return 1;
    }

    quat::app::add_default_lighting(scene, !world.from_gltf());
    // Atmosphere LUTs + dynamic IBL probe, lit by the scene's sun.
    renderer.create_environment(scene, *pipeline_cache, shader_dir);

    // Terrain, navmesh, networking (QUAT_HOST / QUAT_JOIN) and QUAT_TERRAIN.
    world.init(device, allocator, *pipeline_cache, shader_dir, scene);
#ifdef ENGINE_EDITOR
    std::optional<engine::editor::EditorContext::GroundClick> ground_click; // 13.4
#endif

    // Free-fly view camera, seeded by framing the scene (see fly_camera.hpp).
    quat::app::FlyCamera camera = quat::app::FlyCamera::create(scene, physics_mode);

    // ---- Per-frame command + sync resources --------------------------------
    renderer.create_frame_sync();

    // ---- Main loop ---------------------------------------------------------
    // QUAT_MAX_FRAMES, when set, auto-quits after that many presented frames.
    // Used for headless smoke tests so clean teardown can be verified.
    std::uint64_t max_frames = 0;
    if (const char* env = std::getenv("QUAT_MAX_FRAMES"); env != nullptr) {
        max_frames = std::strtoull(env, nullptr, 10);
    }
    // QUAT_SCREENSHOT, combined with QUAT_MAX_FRAMES, writes the final presented
    // frame to the given path as binary PPM — headless visual verification.
    if (const char* screenshot_path = std::getenv("QUAT_SCREENSHOT")) {
        renderer.set_screenshot_path(std::string(screenshot_path));
    }

    std::uint64_t presented = 0;
    double        frame_time_sum = 0.0;     // avg-frame-time report at exit
    std::uint64_t frame_time_samples = 0;
    bool running = true;
    std::uint64_t last_ticks = SDL_GetTicksNS();

    // Remote control (ROADMAP A2/A3): QUAT_REMOTE=<port> opens the loopback
    // JSON-RPC endpoint the quat-mcp bridge talks to (QUAT_REMOTE_TOKEN
    // requires session.hello). The bridge also resolves Primitive components
    // every frame, endpoint or not. See src/app/remote_bridge.hpp.
    quat::app::RemoteBridgeTargets remote_targets;
    remote_targets.device = &device;
    remote_targets.allocator = &allocator;
    remote_targets.assets = &assets;
    remote_targets.scene = &scene;
    remote_targets.frame_counter = &presented;
    remote_targets.cam_pos = &camera.pos;
    remote_targets.cam_yaw = &camera.yaw;
    remote_targets.cam_pitch = &camera.pitch;
    remote_targets.cam_fov_y = &camera.fov_y;
    world.fill_remote_targets(remote_targets);
#ifdef ENGINE_EDITOR
    remote_targets.editor = &editor;
    remote_targets.render_settings = &render_settings;
    remote_targets.play = world.has_demo_physics() ? nullptr : &editor_play; // demo modes own physics
#endif
    remote_targets.project_root = QUAT_PROJECT_ROOT;
    quat::app::RemoteBridge remote_bridge(std::move(remote_targets), k_frames_in_flight);

    world.init_audio(QUAT_PROJECT_ROOT);

    while (running) {
        camera.begin_input(); // per-frame accumulated mouse-look delta

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
#ifdef ENGINE_EDITOR
            editor.process_event(event);
#endif
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                renderer.request_swapchain_recreate();
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION: {
                bool accept_clicks = true;
#ifdef ENGINE_EDITOR
                // Camera input belongs to the Viewport panel; clicks anywhere
                // else are the UI's.
                accept_clicks = !(editor.active() && !editor.viewport_hovered());
#endif
                camera.handle_event(event, window, accept_clicks);
                break;
            }
            default:
                break;
            }
        }
        if (!running) break;

        // Remote-control requests (and primitive resolution) run here: after
        // input, before play-mode transitions, scene file requests and the
        // scene tick.
        remote_bridge.poll();

#ifdef ENGINE_EDITOR
        editor.begin_frame();

        // Play/Stop transition: Play snapshots the scene and brings up a
        // dedicated physics world; Stop tears the world down and restores the
        // snapshot (exact entity ids — selection and hierarchy stay valid).
        if (editor_play != editor_play_prev) {
            if (editor_play) {
                play_snapshot = engine::editor::capture_scene(scene);
                // Unity convention: a Collider without a RigidBody simulates as
                // a static body (kinematic on nav agents, none on characters).
                // Added AFTER the snapshot, so Stop removes it.
                (void)engine::scene::promote_collider_only_bodies(scene.registry());
                if (auto started = world.begin_play(scene); started) {
                    std::fprintf(stderr, "[editor] play (%zu colliders, %zu bodies)\n",
                                 scene.registry().view<engine::scene::Collider>().size(),
                                 scene.registry().view<engine::scene::RigidBody>().size());
                } else {
                    std::fprintf(stderr, "[editor] play failed: %s\n",
                                 started.error().message.c_str());
                    editor_play = false;
                    play_snapshot.reset();
                }
            } else {
                world.end_play();
                if (play_snapshot) {
                    engine::editor::restore_scene(scene, *play_snapshot);
                    play_snapshot.reset();
                }
                std::fprintf(stderr, "[editor] stop\n");
            }
            editor_play_prev = editor_play;
        }

        // Scene save/load requested from the File menu. Saving is pure CPU;
        // loading clears the scene and needs an upload context for asset
        // re-resolution, then the loop's cached entities are rebound.
        if (!pending_save.empty()) {
            const std::string path = std::exchange(pending_save, {});
            if (auto r = engine::scene::save_scene(scene, path); r) {
                std::fprintf(stderr, "[editor] saved scene: %s\n", path.c_str());
            } else {
                std::fprintf(stderr, "[editor] save failed: %s\n", r.error().message.c_str());
            }
        }
        if (!pending_load.empty() && !editor_play) {
            const std::string path = std::exchange(pending_load, {});
            VkCommandPool load_pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = device.queue_families().graphics;
            if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &load_pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext load_transfer{
                    vk_device, load_pool, device.graphics_queue(),
                    device.max_sampler_anisotropy()};
                if (auto r = engine::scene::load_scene(scene, path, allocator, load_transfer,
                                                       assets);
                    r) {
                    std::fprintf(stderr, "[editor] loaded scene: %s\n", path.c_str());
                    world.invalidate_navmesh("scene loaded");
                    // Rebind the loop's cached entities to the loaded scene.
                    // (The view camera is editor state — nothing to rebind.)
                    world.clear_spin_entity();
                    editor.set_selected(entt::null);
                    // The dynamic IBL probe picks up the loaded scene's sun
                    // within one refresh cycle — no explicit rebake needed.
                } else {
                    std::fprintf(stderr, "[editor] load failed: %s\n",
                                 r.error().message.c_str());
                }
                vkDestroyCommandPool(vk_device, load_pool, nullptr);
            }
        }

        // glTF instantiate requested from the asset browser (double-click or
        // viewport drop): blocking load through a transient upload context on
        // the graphics queue (mip blits), like the startup path.
        if (!pending_instantiate.empty()) {
            const std::string path = std::exchange(pending_instantiate, {});
            VkCommandPool load_pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = device.queue_families().graphics;
            if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &load_pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext load_transfer{
                    vk_device, load_pool, device.graphics_queue(),
                    device.max_sampler_anisotropy()};
                if (auto roots = engine::scene::GltfLoader::instantiate(
                        path, allocator, load_transfer, assets, scene);
                    roots) {
                    // Place the new roots in front of the camera (instead of at
                    // their authored origin, which may be out of view) and
                    // select the first so the gizmo picks it up immediately.
                    const glm::vec3 spawn = camera.pos + camera.forward() * 5.0F;
                    for (const entt::entity root : *roots) {
                        auto& t = scene.registry().get<engine::scene::Transform>(root);
                        t.local = glm::translate(glm::mat4(1.0F), spawn) * t.local;
                    }
                    if (!roots->empty()) editor.set_selected(roots->front());
                    std::fprintf(stderr, "[editor] instantiated %s (%zu roots)\n", path.c_str(),
                                 roots->size());
                } else {
                    std::fprintf(stderr, "[editor] instantiate failed: %s\n",
                                 roots.error().message.c_str());
                }
                vkDestroyCommandPool(vk_device, load_pool, nullptr);
            }
        }
#endif

        const VkExtent2D pixel_extent = window_pixel_extent(window);
        if (pixel_extent.width == 0 || pixel_extent.height == 0) {
            SDL_Delay(10); // minimized — nothing to draw
            continue;
        }

        if (!renderer.ensure_swapchain(pixel_extent)) break;

        const std::uint32_t frame = renderer.wait_for_slot();
        remote_bridge.on_frame_complete(frame); // the slot's screenshot readback landed

        const auto acquired = renderer.acquire_and_begin();
        if (acquired == quat::app::FrameRenderer::Acquire::retry) continue;
        if (acquired == quat::app::FrameRenderer::Acquire::failed) break;

        // The scene renders at `draw_extent`; the swapchain presents at
        // `present_extent`. They differ only in editor mode, where the scene
        // fills the Viewport panel (sized by last frame's UI) instead of the
        // whole window.
        const VkExtent2D present_extent = renderer.present_extent();
        VkExtent2D draw_extent = present_extent;
#ifdef ENGINE_EDITOR
        if (editor.active() && editor.viewport_width() > 0 && editor.viewport_height() > 0) {
            draw_extent.width = std::clamp(editor.viewport_width(), 16U, 4096U);
            draw_extent.height = std::clamp(editor.viewport_height(), 16U, 4096U);
        }
#endif
        const float aspect =
            draw_extent.height == 0
                ? 1.0F
                : static_cast<float>(draw_extent.width) / static_cast<float>(draw_extent.height);

        // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift = fast.
        const std::uint64_t now_ticks = SDL_GetTicksNS();
        const float dt = static_cast<float>(now_ticks - last_ticks) * 1e-9F;
        last_ticks = now_ticks;
        renderer.advance_time(dt);

        camera.apply_look();
        quat::app::FlyInputGate gate;
#ifdef ENGINE_EDITOR
        // Editor mode: WASD flies only during a right-drag (W/E/R switch the
        // gizmo otherwise); the ARROW keys fly whenever the cursor is over the
        // viewport (they conflict with nothing). Text input freezes both.
        const bool typing = editor.wants_text_input();
        gate.wasd = !typing && (!editor.active() || camera.looking() || character_mode);
        gate.arrows =
            !typing && (!editor.active() || camera.looking() || editor.viewport_hovered());
#endif

        if (character_mode) {
            world.drive_character(scene, camera.character_wish(gate), dt);
        } else {
            camera.fly(scene, gate, dt);
        }

        const quat::app::PlayStep* play = nullptr;
#ifdef ENGINE_EDITOR
        quat::app::PlayStep play_step;
        if (editor_play) {
            play_step.use_scene_camera = play_use_scene_camera;
            play_step.fly_camera_pos = camera.pos;
            play = &play_step;
        }
#endif
        world.simulate(scene, presented, dt, play);

        // Third-person follow camera, placed after the character has moved.
        if (character_mode) camera.follow(scene, world.character_entity());
#ifdef ENGINE_EDITOR
        // The editor's fly camera is the default view in BOTH modes — Play
        // must never take movement away from the user (old scene files also
        // carry stale "camera" entities that would hijack the view). The
        // menu-bar "Game camera" checkbox opts the play view into the scene's
        // active Camera entity (a camera parented to a moving object follows
        // it); it falls back to the fly camera when none is active.
        const bool through_scene_camera =
            editor_play && play_use_scene_camera &&
            engine::scene::find_active_camera(scene.registry()) != entt::null;
        const engine::scene::CameraMatrices cam =
            through_scene_camera
                ? engine::scene::camera_system(scene.registry(), aspect)
                : camera.matrices(aspect);
        world.update_audio(scene, cam.view, editor_play, dt);
#else
        const engine::scene::CameraMatrices cam =
            engine::scene::camera_system(scene.registry(), aspect);
        world.update_audio(scene, cam.view, /*live=*/true, dt);
#endif

        const quat::app::FrameScene frame_scene =
            renderer.gather(scene, cam, draw_extent, presented);

        world.update_terrain(scene, cam.position);

#ifdef ENGINE_EDITOR
        // Ctrl/Shift+click in the viewport (13.4): resolve the ray against the
        // terrain (bisected raymarch) or the ground plane, then command the
        // selected entity: Ctrl = walk here now, Shift = append a patrol point.
        if (ground_click) {
            const auto click = *ground_click;
            ground_click.reset();
            const auto& height_at = world.ground_height(); // shared sampler
            std::optional<glm::vec3> hit;
            float prev_t = 0.0F;
            for (float t = 2.0F; t < 2000.0F && !hit; t += 2.0F) {
                const glm::vec3 pos = click.origin + click.dir * t;
                const auto h = height_at(pos.x, pos.z);
                if (h && pos.y <= *h) {
                    // Bisect between the last point above and this one below.
                    float lo = prev_t;
                    float hi = t;
                    for (int i = 0; i < 16; ++i) {
                        const float mid = 0.5F * (lo + hi);
                        const glm::vec3 mp = click.origin + click.dir * mid;
                        const auto mh = height_at(mp.x, mp.z);
                        (mh && mp.y <= *mh ? hi : lo) = mid;
                    }
                    const glm::vec3 hp = click.origin + click.dir * hi;
                    hit = glm::vec3(hp.x, height_at(hp.x, hp.z).value_or(hp.y), hp.z);
                }
                prev_t = t;
            }
            if (!hit && click.dir.y < -1e-4F) { // no terrain: ground plane
                const float t = (world.terrain_anchor().y - click.origin.y) / click.dir.y;
                if (t > 0.0F) hit = click.origin + click.dir * t;
            }
            const entt::entity sel = editor.selected();
            if (hit && scene.registry().valid(sel) &&
                scene.registry().any_of<engine::scene::Transform>(sel)) {
                auto& agent = scene.registry().get_or_emplace<engine::scene::NavAgent>(sel);
                if (click.add_waypoint) {
                    auto& route =
                        scene.registry().get_or_emplace<engine::scene::PatrolRoute>(sel);
                    route.points.push_back(*hit);
                    std::fprintf(stderr, "[nav] waypoint %zu at (%.1f, %.1f, %.1f)\n",
                                 route.points.size(), hit->x, hit->y, hit->z);
                } else {
                    engine::scene::nav_agent_command(agent, *hit);
                    std::fprintf(stderr, "[nav] go to (%.1f, %.1f, %.1f)\n", hit->x, hit->y,
                                 hit->z);
                }
            }
        }
#endif

        world.service_navmesh_request(scene);

        quat::app::FrameInputs frame_inputs;
        frame_inputs.scene = &frame_scene;
        frame_inputs.cam = &cam;
        frame_inputs.draw_extent = draw_extent;
        frame_inputs.dt = dt;
        if (world.terrain_drawable()) {
            frame_inputs.terrain = &world.terrain_pass();
            frame_inputs.terrain_anchor = world.terrain_anchor();
            frame_inputs.terrain_snowline = world.terrain_snowline();
        }
#ifdef ENGINE_EDITOR
        // Editor UI: build the dockspace + panels (the Viewport panel samples
        // the offscreen scene image), then draw the UI into the swapchain as
        // the last pass. A null/empty draw list (editor disabled) adds no pass.
        frame_inputs.build_ui = [&]() -> const ImDrawData* {
            engine::editor::EditorContext ui_ctx;
            ui_ctx.frame_ms = dt * 1000.0F;
            ui_ctx.draw_count =
                static_cast<int>(frame_scene.draws.size() + frame_scene.transparent_draws.size());
            ui_ctx.entity_count = static_cast<int>(
                scene.registry().view<engine::scene::Transform>().size());
            ui_ctx.scene = &scene;
            ui_ctx.renderer = &render_settings;
            ui_ctx.project_root = QUAT_PROJECT_ROOT;
            ui_ctx.instantiate_request = &pending_instantiate;
            ui_ctx.save_request = &pending_save;
            ui_ctx.load_request = &pending_load;
            // Demo modes own their physics world — hide the Play button there.
            ui_ctx.play_mode = world.has_demo_physics() ? nullptr : &editor_play;
            ui_ctx.play_use_scene_camera =
                world.has_demo_physics() ? nullptr : &play_use_scene_camera;
            ui_ctx.build_navmesh_request = &world.navmesh().request;
            ui_ctx.ground_click = &ground_click;
            ui_ctx.nav_edges =
                world.navmesh().mesh.valid() ? &world.navmesh().mesh.debug_edges() : nullptr;
            ui_ctx.nav_status = &world.navmesh().status;
            ui_ctx.view_proj = cam.view_proj; // unjittered, for overlays
            ui_ctx.view = cam.view;
            ui_ctx.proj_no_flip = cam.projection;
            ui_ctx.proj_no_flip[1][1] *= -1.0F; // undo the Vulkan Y flip for ImGuizmo
            editor.build_ui(ui_ctx);
            return editor.end_frame();
        };
#endif
        if (!renderer.record(frame_inputs)) break;

        remote_bridge.record_capture(renderer.command_buffer(), renderer.frame_slot(),
                                     renderer.capture_source());

        if (!renderer.submit_and_present(max_frames != 0 && presented + 1 >= max_frames)) {
            break;
        }

        // Average frame time over the run, skipping the first frame (it carries
        // one-time pipeline/descriptor warmup). Reported at exit.
        if (presented > 0) {
            frame_time_sum += dt;
            frame_time_samples += 1;
        }

        // Advances every frame (audit A1): drives TAA jitter, the demo cube,
        // frame stats and engine.info — not only QUAT_MAX_FRAMES runs.
        ++presented;
        if (max_frames != 0 && presented >= max_frames) {
            running = false;
        }
    }

    if (frame_time_samples > 0) {
        const double avg_ms = frame_time_sum / static_cast<double>(frame_time_samples) * 1000.0;
        std::fprintf(stderr, "[stats] avg frame %.2f ms (%.1f fps) over %llu frames\n", avg_ms,
                     avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0,
                     static_cast<unsigned long long>(frame_time_samples));
    }

    // ---- Teardown ----------------------------------------------------------
    vkDeviceWaitIdle(vk_device);
    world.shutdown(); // waits for a running terrain job, frees its upload pool
    renderer.destroy_frame_sync();
    swapchain = engine::rhi::Swapchain{};     // destroy swapchain before surface
    vkDestroySurfaceKHR(device.instance(), surface, nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
