#include "app/world_runtime.hpp"

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "app/frame_renderer.hpp" // k_frames_in_flight
#include "app/nav_bake.hpp"
#include "app/remote_bridge.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

WorldRuntime::WorldRuntime() {
    // Authoritative ground height at world (x, z): streamed tiles first,
    // then the single editing tile. Shared by kinematic nav agents (the
    // coarse navmesh floats off the real surface) and ground clicks.
    ground_height_at_ = [this](float x, float z) -> std::optional<float> {
        if (terrain_streamer_) {
            const float tile = terrain_streamer_->base().tile_size_m;
            const glm::ivec2 coord(
                static_cast<int>(std::floor((x - terrain_anchor_.x) / tile + 0.5F)),
                static_cast<int>(std::floor((z - terrain_anchor_.z) / tile + 0.5F)));
            const auto it = terrain_streamer_->maps().find(coord);
            if (it == terrain_streamer_->maps().end()) return std::nullopt;
            const glm::vec3 org = terrain_streamer_->tile_origin(coord, terrain_anchor_);
            return org.y + it->second.sample(x - org.x, z - org.z);
        }
        if (terrain_present_ && terrain_heightmap_.valid()) {
            const float half = terrain_heightmap_.tile_size_m * 0.5F;
            const float lx = x - (terrain_anchor_.x - half);
            const float lz = z - (terrain_anchor_.z - half);
            if (lx < 0.0F || lz < 0.0F || lx > terrain_heightmap_.tile_size_m ||
                lz > terrain_heightmap_.tile_size_m) {
                return std::nullopt;
            }
            return terrain_anchor_.y + terrain_heightmap_.sample(lx, lz);
        }
        return std::nullopt;
    };

    // Ground under a foot (FootIK): a physics raycast in the running world
    // (skipping the character's own colliders), else the terrain height.
    foot_probe_ = [this](const glm::vec3& origin, float length,
                         std::span<const entt::entity> ignore)
        -> std::optional<engine::animation::GroundHit> {
        const engine::physics::PhysicsWorld* world =
            play_physics_         ? &*play_physics_
            : demo_.physics_world ? &*demo_.physics_world
                                  : nullptr;
        if (world != nullptr) {
            std::vector<std::uint32_t>& owners = probe_owners_; // reused buffer
            owners.clear();
            for (const entt::entity e : ignore) {
                owners.push_back(static_cast<std::uint32_t>(entt::to_integral(e)));
            }
            if (const auto hit = world->cast_ray(origin, glm::vec3(0.0F, -1.0F, 0.0F), length,
                                                 false, owners)) {
                return engine::animation::GroundHit{hit->point, hit->normal};
            }
        }
        const std::optional<float> h = ground_height_at_(origin.x, origin.z);
        if (!h || *h > origin.y || *h < origin.y - length) return std::nullopt;
        constexpr float e = 0.25F; // normal from central differences
        const auto at = [&](float x, float z) { return ground_height_at_(x, z).value_or(*h); };
        const glm::vec3 normal = glm::normalize(
            glm::vec3(at(origin.x - e, origin.z) - at(origin.x + e, origin.z), 2.0F * e,
                      at(origin.x, origin.z - e) - at(origin.x, origin.z + e)));
        return engine::animation::GroundHit{glm::vec3(origin.x, *h, origin.z), normal};
    };
}

WorldRuntime::~WorldRuntime() {
    shutdown();
}

bool WorldRuntime::build_scene(engine::scene::Scene& scene, engine::asset::AssetManager& assets,
                               engine::rhi::GpuAllocator& allocator,
                               const engine::rhi::TransferContext& setup_transfer) {
    auto demo_result = build_demo_scene(scene, assets, allocator, setup_transfer);
    if (!demo_result) return false;
    demo_ = std::move(*demo_result);
    return true;
}

void WorldRuntime::init(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                        engine::rhi::PipelineCache& pipeline_cache, const std::string& shader_dir,
                        engine::scene::Scene& scene) {
    // Terrain (Phase 12): the pass owns the heightmap texture + LOD index
    // buffers; the frame loop generates async whenever a Terrain component
    // asks and uploads the result here. Uploads need a transfer pool that
    // outlives setup (setup_pool is destroyed after scene construction).
    device_ = device.handle();
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(device_, &info, nullptr, &terrain_pool_);
    }
    terrain_transfer_ = engine::rhi::TransferContext{device_, terrain_pool_,
                                                     device.graphics_queue(),
                                                     device.max_sampler_anisotropy()};
    if (auto tp = engine::renderer::TerrainPass::create(device, allocator, pipeline_cache,
                                                        terrain_transfer_, shader_dir);
        tp) {
        terrain_pass_ = std::move(*tp);
    } else {
        std::fprintf(stderr, "[warn] terrain pass create failed: %s\n", tp.error().message.c_str());
    }
    grass_.emplace(device, allocator, pipeline_cache, terrain_transfer_, shader_dir,
                   k_frames_in_flight);
    particles_.emplace(device, allocator, pipeline_cache, terrain_transfer_, shader_dir,
                       QUAT_PROJECT_ROOT, k_frames_in_flight);

    net_ = engine::net::NetSession::from_environment(scene.registry());

    // QUAT_TERRAIN=1: drop a procedural terrain tile under the showcase scene
    // (sunken so its mid heights sit near the platform) — exercises the full
    // async generate -> upload -> chunked LOD draw path without the editor.
    add_env_terrain(scene);
}

void WorldRuntime::init_audio(const std::string& asset_root) {
    audio_.emplace(AudioRuntime::create(asset_root));
}

void WorldRuntime::fill_remote_targets(RemoteBridgeTargets& targets) {
    targets.ground_height = &ground_height_at_;
    targets.terrain_streamer = &terrain_streamer_;
    targets.terrain_heightmap = &terrain_heightmap_;
    targets.terrain_anchor = &terrain_anchor_;
    targets.navmesh = &nav_.mesh;
    targets.spin_entity = &demo_.spin_entity;
    targets.demo_physics = &demo_.physics_world;
#ifdef ENGINE_EDITOR
    targets.play_physics = &play_physics_;
#endif
}

void WorldRuntime::shutdown() {
    // Frames no longer run, so a GPU tile would never be delivered (audit
    // D1): abandon it, and stop a CPU job at its next erosion step, before
    // waiting on the job.
    abandon_terrain_job();
    if (terrain_job_.valid()) terrain_job_.wait(); // worker may still hold the params
    if (terrain_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, terrain_pool_, nullptr);
        terrain_pool_ = VK_NULL_HANDLE;
    }
}

void WorldRuntime::abandon_terrain_job() {
    terrain_pass_.abandon_pending_tiles();
    if (terrain_cancel_) terrain_cancel_->store(true);
}

void WorldRuntime::drive_character(engine::scene::Scene& scene, const glm::vec3& wish, float dt) {
    // Offline this sets the controller's move; networked, NetSession
    // predicts it (client) or leaves it to the remote player (host).
    net_.drive_character(scene.registry(),
                         demo_.physics_world ? &*demo_.physics_world : nullptr,
                         demo_.character_entity, wish, dt);
}

void WorldRuntime::simulate(engine::scene::Scene& scene, std::uint64_t frame_number, float dt,
                            float sim_dt, const PlayStep* play) {
    // Spin the demo cube (if present — the editor may have deleted it).
    if (demo_.spin_entity != entt::null) {
        auto* spin_tf =
            scene.registry().valid(demo_.spin_entity)
                ? scene.registry().try_get<engine::scene::Transform>(demo_.spin_entity)
                : nullptr;
        if (spin_tf == nullptr) {
            demo_.spin_entity = entt::null; // deleted in the editor — stop driving it
        } else {
            spin_tf->local =
                glm::translate(glm::mat4(1.0F), demo_.spin_pos) *
                glm::rotate(glm::mat4(1.0F), static_cast<float>(frame_number) * 0.02F,
                            glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F))) *
                glm::scale(glm::mat4(1.0F), glm::vec3(demo_.spin_scale));
        }
    }

    // Networking before the simulation: the host steps the remote player's
    // inputs, a joined client reconciles and applies replicated poses (so
    // this frame's physics and draw list see them).
    net_.update(scene.registry(), demo_.physics_world ? &*demo_.physics_world : nullptr,
                demo_.character_entity, dt);
    if (demo_.physics_world) {
        if (demo_.character_mode) {
            engine::scene::character_system(scene.registry(), *demo_.physics_world, sim_dt);
        } else {
            engine::scene::physics_system(scene.registry(), *demo_.physics_world, sim_dt);
        }
    } else if (play != nullptr && play_physics_) {
        // Perception: the trees see the user's view — the fly camera,
        // unless Play looks through the scene's active Camera entity
        // (then near_camera falls back to that entity).
        const bool viewing_scene_camera =
            play->use_scene_camera &&
            engine::scene::find_active_camera(scene.registry()) != entt::null;
        engine::scene::bt_system(
            scene.registry(), sim_dt,
            {.navmesh = &nav_.mesh,
             .ground_height = ground_height_at_,
             .viewer = viewing_scene_camera ? std::nullopt
                                            : std::optional(play->fly_camera_pos)});
        if (std::exchange(nav_.auto_bake, false) && !nav_.mesh.valid() &&
            !scene.registry().view<engine::scene::NavAgent>().empty()) {
            nav_.request = true; // baked below, this frame
            std::fprintf(stderr, "[nav] no navmesh for the agents: baking one\n");
        }
        engine::scene::nav_agent_system(scene.registry(), nav_.mesh, sim_dt, ground_height_at_);
        engine::scene::character_system(scene.registry(), *play_physics_, sim_dt);
        engine::scene::physics_system(scene.registry(), *play_physics_, sim_dt);
    }
    scene.tick(sim_dt, foot_probe_); // advances any Animators by simulated time

    // Replication: the host broadcasts moved entities (fresh world poses).
    net_.publish(scene.registry(), dt);
}

void WorldRuntime::update_audio(engine::scene::Scene& scene, const glm::mat4& view, bool live,
                                float dt) {
    if (audio_) audio_->update(scene.registry(), view, live, dt);
}

void WorldRuntime::update_terrain(engine::scene::Scene& scene, const glm::vec3& camera_pos) {
    // Terrain upkeep: single-tile mode generates async on demand; streaming
    // mode keeps a seamless tile window centred on the camera (12.4). Both
    // upload before this frame's graph builds, without pipeline stalls.
    // The pass's upkeep runs every frame, terrain or not: it frees retired
    // tiles and finished GPU jobs, and stops GPU work nobody wants any more.
    terrain_pass_.tick();
    terrain_anchor_ = glm::vec3(0.0F);
    terrain_snowline_ = 110.0F;
    terrain_present_ = false;
    terrain_stream_mode_ = false;
    for (auto&& [te, tcomp] : scene.registry().view<engine::scene::Terrain>().each()) {
        terrain_present_ = true;
        terrain_stream_mode_ = tcomp.streaming;
        const auto* tf = scene.registry().try_get<engine::scene::Transform>(te);
        terrain_anchor_ = tf != nullptr ? glm::vec3(tf->world[3]) : glm::vec3(0.0F);
        terrain_snowline_ = tcomp.snowline_m;

        if (tcomp.streaming) {
            if (tcomp.regenerate || !terrain_streamer_) {
                tcomp.regenerate = false;
                // Tear down whichever mode ran before this configuration.
                if (terrain_streamer_) {
                    for (const auto& [c, m] : terrain_streamer_->maps()) {
                        terrain_pass_.remove_tile(c);
                    }
                }
                terrain_pass_.remove_tile(glm::ivec2(0, 0));
                terrain_streamer_.emplace(tcomp.params, tcomp.stream_radius);
                terrain_streamer_->set_generator( // R10.2 GPU synthesis
                    tcomp.gpu_generate ? terrain_pass_.tile_generator() : nullptr);
                invalidate_navmesh("terrain regenerated");
                std::fprintf(stderr, "[terrain] streaming world (seed %u, radius %d)\n",
                             tcomp.params.seed, tcomp.stream_radius);
            }
        } else {
            if (terrain_streamer_) {
                for (const auto& [c, m] : terrain_streamer_->maps()) {
                    terrain_pass_.remove_tile(c);
                }
                terrain_streamer_.reset();
                tcomp.regenerate = true; // rebuild the fixed tile
            }
            // A regenerate supersedes a job still running (slider drags):
            // cancel it so the new parameters do not queue behind it; the
            // cancelled result arrives invalid and is skipped below.
            if (tcomp.regenerate && terrain_job_.valid() && terrain_cancel_) {
                terrain_cancel_->store(true);
            }
            if (tcomp.regenerate && !terrain_job_.valid()) {
                tcomp.regenerate = false;
                terrain_cancel_ = engine::terrain::make_cancel_token();
                terrain_job_ = engine::terrain::generate_tile_async( // R10.2: GPU or CPU
                    tcomp.gpu_generate ? terrain_pass_.tile_generator() : nullptr,
                    tcomp.params, terrain_cancel_);
                std::fprintf(stderr, "[terrain] generating %ux%u tile (seed %u)...\n",
                             tcomp.params.resolution, tcomp.params.resolution,
                             tcomp.params.seed);
            }
        }
        break; // a single Terrain entity drives the world
    }
    if (terrain_streamer_ && terrain_present_ && terrain_stream_mode_) {
        const auto ev = terrain_streamer_->update(
            glm::vec2(camera_pos.x - terrain_anchor_.x, camera_pos.z - terrain_anchor_.z));
        for (const glm::ivec2& c : ev.removed) {
            terrain_pass_.remove_tile(c);
            if (play_physics_ && terrain_bodies_.contains(c)) {
                play_physics_->remove_body(terrain_bodies_.at(c));
                terrain_bodies_.erase(c);
            }
        }
        for (const glm::ivec2& c : ev.ready) {
            const engine::terrain::Heightmap& map = terrain_streamer_->maps().at(c);
            if (auto r = terrain_pass_.set_tile(c, map, terrain_transfer_); r) {
                std::fprintf(stderr, "[terrain] tile (%d,%d) streamed in\n", c.x, c.y);
            } else {
                std::fprintf(stderr, "[terrain] tile upload failed: %s\n",
                             r.error().message.c_str());
            }
            if (play_physics_) {
                const std::uint32_t body = engine::scene::add_height_field_body(
                    *play_physics_, map, terrain_streamer_->tile_origin(c, terrain_anchor_));
                if (body != engine::physics::PhysicsWorld::invalid_body) {
                    terrain_bodies_.emplace(c, body);
                }
            }
        }
    }
    // The Terrain entity went away mid-generation: nothing wants the tile,
    // and a GPU job would not progress without the terrain pass (audit D1).
    if (!terrain_present_ && terrain_job_.valid()) abandon_terrain_job();
    if (terrain_job_.valid() &&
        terrain_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        engine::terrain::Heightmap map = terrain_job_.get();
        if (!map.valid()) return; // cancelled / abandoned
        terrain_heightmap_ = std::move(map);
        invalidate_navmesh("terrain regenerated");
        if (auto r = terrain_pass_.set_tile(glm::ivec2(0, 0), terrain_heightmap_,
                                            terrain_transfer_);
            r) {
            std::fprintf(stderr, "[terrain] tile ready (%.0f m, heights %.1f..%.1f m)\n",
                         terrain_heightmap_.tile_size_m, terrain_heightmap_.min_height,
                         terrain_heightmap_.max_height);
        } else {
            std::fprintf(stderr, "[terrain] upload failed: %s\n", r.error().message.c_str());
        }
    }
}

void WorldRuntime::service_navmesh_request(engine::scene::Scene& scene) {
    // Navmesh bake requested from the Physics panel: gather the static
    // walkable geometry (terrain + static box colliders) and run Recast.
    // Blocking, but explicit and logged (editor tooling).
    if (!nav_.request) return;
    nav_.request = false;
    if (auto built = bake_navmesh(scene.registry(),
                                  terrain_streamer_ ? &*terrain_streamer_ : nullptr,
                                  &terrain_heightmap_, terrain_anchor_);
        built) {
        nav_.mesh = std::move(*built);
        const std::size_t edges = nav_.mesh.debug_edges().size();
        nav_.status = "baked: " + std::to_string(edges) + " edges";
    } else {
        // Never keep the old mesh silently: it no longer matches the scene.
        nav_.mesh = {};
        nav_.status = "bake failed: " + built.error().message;
        std::fprintf(stderr, "[nav] bake failed (navmesh cleared): %s\n",
                     built.error().message.c_str());
    }
}

std::expected<void, engine::core::Error> WorldRuntime::begin_play(engine::scene::Scene& scene) {
    auto world = engine::physics::PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());
    play_physics_ = std::move(*world);
    // Terrain joins the simulation as static Jolt height
    // fields matching the rendered surface (12.3/12.4).
    if (terrain_streamer_) {
        glm::vec3 anchor(0.0F);
        for (auto&& [te2, t2] : scene.registry().view<engine::scene::Terrain>().each()) {
            const auto* tf2 = scene.registry().try_get<engine::scene::Transform>(te2);
            anchor = tf2 != nullptr ? glm::vec3(tf2->world[3]) : glm::vec3(0.0F);
            break;
        }
        for (const auto& [c, m] : terrain_streamer_->maps()) {
            const std::uint32_t body = engine::scene::add_height_field_body(
                *play_physics_, m, terrain_streamer_->tile_origin(c, anchor));
            if (body != engine::physics::PhysicsWorld::invalid_body) {
                terrain_bodies_.emplace(c, body);
            }
        }
        std::fprintf(stderr, "[terrain] %zu height-field bodies added\n", terrain_bodies_.size());
    } else if (terrain_heightmap_.valid()) {
        if (engine::scene::add_terrain_body(scene.registry(), *play_physics_,
                                            terrain_heightmap_) !=
            engine::physics::PhysicsWorld::invalid_body) {
            std::fprintf(stderr, "[terrain] height-field body added\n");
        }
    }
    nav_.auto_bake = true; // bake now if agents have no mesh
    return {};
}

void WorldRuntime::end_play() {
    play_physics_.reset();
    terrain_bodies_.clear(); // died with the play world
}

void WorldRuntime::invalidate_navmesh(const char* why) {
    nav_.auto_bake = true;
    if (!nav_.mesh.valid()) return;
    nav_.mesh = {};
    nav_.status = std::string("cleared: ") + why + " (rebake)";
    std::fprintf(stderr, "[nav] navmesh cleared: %s\n", why);
}

} // namespace quat::app
