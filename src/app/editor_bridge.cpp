#include "app/editor_bridge.hpp"

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <cstdio>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "app/asset_root.hpp"
#include "app/remote_bridge.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/navigation.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"

namespace quat::app {
namespace {

// Runs `fn` with a transient upload context on the graphics queue (mip
// blits need a graphics-capable queue), like the startup path. Skipped if the
// command pool cannot be created.
template <typename Fn>
void with_load_transfer(const engine::rhi::Device& device, Fn&& fn) {
    const VkDevice vk_device = device.handle();
    VkCommandPool load_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &load_pool) == VK_SUCCESS) {
        const engine::rhi::TransferContext load_transfer{
            vk_device, load_pool, device.graphics_queue(), device.max_sampler_anisotropy(),
            device.texture_caps()};
        std::forward<Fn>(fn)(load_transfer);
        vkDestroyCommandPool(vk_device, load_pool, nullptr);
    }
}

} // namespace

void EditorBridge::create(SDL_Window* window, bool enabled) {
    if (!enabled) return;
    if (auto e = engine::editor::EditorLayer::create(
            window, engine::core::path_to_utf8(project_root() / "assets" / "fonts" / "Roboto-Medium.ttf"));
        e) {
        editor_ = std::move(*e);
    } else {
        std::fprintf(stderr, "[warn] editor disabled: %s\n", e.error().message.c_str());
    }
}

void EditorBridge::fill_remote_targets(RemoteBridgeTargets& targets, bool demo_physics) {
    targets.editor = &editor_;
    targets.play = demo_physics ? nullptr : &play_; // demo modes own physics
}

void EditorBridge::process_event(const SDL_Event& event) {
    editor_.process_event(event);
}

FlyInputGate EditorBridge::input_gate(bool looking, bool character_mode) const {
    const bool typing = editor_.wants_text_input();
    FlyInputGate gate;
    gate.wasd = !typing && (!editor_.active() || looking || character_mode);
    gate.arrows = !typing && (!editor_.active() || looking || editor_.viewport_hovered());
    return gate;
}

VkExtent2D EditorBridge::scene_extent(VkExtent2D present_extent) const {
    VkExtent2D draw_extent = present_extent;
    if (editor_.active() && editor_.viewport_width() > 0 && editor_.viewport_height() > 0) {
        draw_extent.width = std::clamp(editor_.viewport_width(), 16U, 4096U);
        draw_extent.height = std::clamp(editor_.viewport_height(), 16U, 4096U);
    }
    return draw_extent;
}

std::optional<PlayStep> EditorBridge::play_step(const glm::vec3& fly_camera_pos) const {
    if (!play_) return std::nullopt;
    PlayStep step;
    step.use_scene_camera = play_use_scene_camera_;
    step.fly_camera_pos = fly_camera_pos;
    return step;
}

bool EditorBridge::through_scene_camera() const {
    return play_ && play_use_scene_camera_ &&
           engine::scene::find_active_camera(t_.scene->registry()) != entt::null;
}

void EditorBridge::begin_frame() {
    editor_.begin_frame();
    apply_play_transition();
    apply_scene_file_requests();
    apply_instantiate_request();
}

void EditorBridge::apply_play_transition() {
    engine::scene::Scene& scene = *t_.scene;
    // Play/Stop transition: Play snapshots the scene and brings up a
    // dedicated physics world; Stop tears the world down and restores the
    // snapshot (exact entity ids — selection and hierarchy stay valid).
    if (play_ == play_prev_) return;
    if (play_) {
        play_snapshot_ = engine::editor::capture_scene(scene);
        // Emitters start over in play (one-shot bursts fire again), as
        // audio sources do (the snapshot clears their runtime state).
        t_.world->reset_particles();
        // Unity convention: a Collider without a RigidBody simulates as
        // a static body (kinematic on nav agents, none on characters).
        // Added AFTER the snapshot, so Stop removes it.
        (void)engine::scene::promote_collider_only_bodies(scene.registry());
        if (auto started = t_.world->begin_play(scene); started) {
            std::fprintf(stderr, "[editor] play (%zu colliders, %zu bodies)\n",
                         scene.registry().view<engine::scene::Collider>().size(),
                         scene.registry().view<engine::scene::RigidBody>().size());
        } else {
            std::fprintf(stderr, "[editor] play failed: %s\n", started.error().message.c_str());
            play_ = false;
            play_snapshot_.reset();
        }
    } else {
        t_.world->end_play();
        if (play_snapshot_) {
            engine::editor::restore_scene(scene, *play_snapshot_);
            play_snapshot_.reset();
        }
        t_.world->reset_particles(); // play's particles vanish with play's scene
        std::fprintf(stderr, "[editor] stop\n");
    }
    play_prev_ = play_;
}

void EditorBridge::apply_scene_file_requests() {
    engine::scene::Scene& scene = *t_.scene;
    // Scene save/load requested from the File menu. Saving is pure CPU;
    // loading clears the scene and needs an upload context for asset
    // re-resolution, then the loop's cached entities are rebound.
    if (!pending_save_.empty()) {
        const std::string path = std::exchange(pending_save_, {});
        if (auto r = engine::scene::save_scene(scene, engine::core::path_from_utf8(path),
                                               t_.render_settings);
            r) {
            std::fprintf(stderr, "[editor] saved scene: %s\n", path.c_str());
        } else {
            std::fprintf(stderr, "[editor] save failed: %s\n", r.error().message.c_str());
        }
    }
    if (!pending_load_.empty() && !play_) {
        const std::string path = std::exchange(pending_load_, {});
        with_load_transfer(*t_.device, [&](const engine::rhi::TransferContext& load_transfer) {
            if (auto r = engine::scene::load_scene(scene, engine::core::path_from_utf8(path),
                                                   *t_.allocator, load_transfer, *t_.assets,
                                                   t_.render_settings);
                r) {
                std::fprintf(stderr, "[editor] loaded scene: %s\n", path.c_str());
                t_.world->invalidate_navmesh("scene loaded");
                // Rebind the loop's cached entities to the loaded scene.
                // (The view camera is editor state — nothing to rebind.)
                t_.world->clear_demo_entities();
                editor_.set_selected(entt::null);
                // The dynamic IBL probe picks up the loaded scene's sun
                // within one refresh cycle — no explicit rebake needed.
            } else {
                std::fprintf(stderr, "[editor] load failed: %s\n", r.error().message.c_str());
            }
        });
    }
}

void EditorBridge::apply_instantiate_request() {
    engine::scene::Scene& scene = *t_.scene;
    // glTF instantiate requested from the asset browser (double-click or
    // viewport drop): blocking load through a transient upload context on
    // the graphics queue (mip blits), like the startup path.
    if (pending_instantiate_.empty()) return;
    const std::string path = std::exchange(pending_instantiate_, {});
    with_load_transfer(*t_.device, [&](const engine::rhi::TransferContext& load_transfer) {
        if (auto roots = engine::scene::GltfLoader::instantiate(engine::core::path_from_utf8(path), *t_.allocator,
                                                                load_transfer, *t_.assets, scene);
            roots) {
            // Place the new roots in front of the camera (instead of at
            // their authored origin, which may be out of view) and
            // select the first so the gizmo picks it up immediately.
            const glm::vec3 spawn = t_.camera->pos + t_.camera->forward() * 5.0F;
            for (const entt::entity root : *roots) {
                auto& t = scene.registry().get<engine::scene::Transform>(root);
                t.local = glm::translate(glm::mat4(1.0F), spawn) * t.local;
            }
            if (!roots->empty()) editor_.set_selected(roots->front());
            std::fprintf(stderr, "[editor] instantiated %s (%zu roots)\n", path.c_str(),
                         roots->size());
        } else {
            std::fprintf(stderr, "[editor] instantiate failed: %s\n",
                         roots.error().message.c_str());
        }
    });
}

void EditorBridge::handle_ground_click() {
    // Ctrl/Shift+click in the viewport (13.4): resolve the ray against the
    // terrain (bisected raymarch) or the ground plane, then command the
    // selected entity: Ctrl = walk here now, Shift = append a patrol point.
    if (!ground_click_) return;
    engine::scene::Scene& scene = *t_.scene;
    const auto click = *ground_click_;
    ground_click_.reset();
    const auto& height_at = t_.world->ground_height(); // shared sampler
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
        const float t = (t_.world->terrain_anchor().y - click.origin.y) / click.dir.y;
        if (t > 0.0F) hit = click.origin + click.dir * t;
    }
    const entt::entity sel = editor_.selected();
    if (hit && scene.registry().valid(sel) &&
        scene.registry().any_of<engine::scene::Transform>(sel)) {
        auto& agent = scene.registry().get_or_emplace<engine::scene::NavAgent>(sel);
        if (click.add_waypoint) {
            auto& route = scene.registry().get_or_emplace<engine::scene::PatrolRoute>(sel);
            route.points.push_back(*hit);
            std::fprintf(stderr, "[nav] waypoint %zu at (%.1f, %.1f, %.1f)\n",
                         route.points.size(), hit->x, hit->y, hit->z);
        } else {
            engine::scene::nav_agent_command(agent, *hit);
            std::fprintf(stderr, "[nav] go to (%.1f, %.1f, %.1f)\n", hit->x, hit->y, hit->z);
        }
    }
}

const ImDrawData* EditorBridge::build_ui(float dt, int draw_count,
                                         const engine::scene::CameraMatrices& cam) {
    engine::scene::Scene& scene = *t_.scene;
    WorldRuntime& world = *t_.world;
    engine::editor::EditorContext ui_ctx;
    ui_ctx.frame_ms = dt * 1000.0F;
    ui_ctx.draw_count = draw_count;
    ui_ctx.entity_count =
        static_cast<int>(scene.registry().view<engine::scene::Transform>().size());
    ui_ctx.shader_statuses = t_.shader_reload != nullptr ? &t_.shader_reload->statuses() : nullptr;
    ui_ctx.scene = &scene;
    ui_ctx.renderer = t_.render_settings;
    ui_ctx.project_root = engine::core::path_to_utf8(project_root()); // UTF-8 for ImGui
    ui_ctx.instantiate_request = &pending_instantiate_;
    ui_ctx.save_request = &pending_save_;
    ui_ctx.load_request = &pending_load_;
    // Demo modes own their physics world — hide the Play button there.
    ui_ctx.play_mode = world.has_demo_physics() ? nullptr : &play_;
    ui_ctx.play_use_scene_camera = world.has_demo_physics() ? nullptr : &play_use_scene_camera_;
    ui_ctx.build_navmesh_request = &world.navmesh().request;
    ui_ctx.ground_click = &ground_click_;
    ui_ctx.nav_edges =
        world.navmesh().mesh.valid() ? &world.navmesh().mesh.debug_edges() : nullptr;
    ui_ctx.nav_status = &world.navmesh().status;
    ui_ctx.view_proj = cam.view_proj; // unjittered, for overlays
    ui_ctx.view = cam.view;
    ui_ctx.proj_no_flip = cam.projection;
    ui_ctx.proj_no_flip[1][1] *= -1.0F; // undo the Vulkan Y flip for ImGuizmo
    editor_.build_ui(ui_ctx);
    return editor_.end_frame();
}

} // namespace quat::app

#endif // ENGINE_EDITOR
