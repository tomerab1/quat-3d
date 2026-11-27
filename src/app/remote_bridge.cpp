// RemoteBridge — see remote_bridge.hpp.

#include "app/remote_bridge.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

#include <entt/entity/registry.hpp>
#include <glm/gtc/constants.hpp>

#include "app/nav_bake.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/core/log.hpp"
#include "engine/core/sim_clock.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/remote/world_commands.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/primitives.hpp"
#include "engine/scene/scatter.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/terrain/heightmap.hpp"
#include "engine/terrain/streamer.hpp"

#ifdef ENGINE_EDITOR
#include "engine/editor/editor.hpp"
#endif

namespace quat::app {

namespace {

namespace remote = engine::remote;
using engine::core::Error;
using remote::Json;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// Process resident set size, where cheaply available.
[[nodiscard]] std::optional<std::uint64_t> resident_bytes() {
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return std::nullopt;
    unsigned long long total = 0;
    unsigned long long resident = 0;
    const int read = std::fscanf(f, "%llu %llu", &total, &resident);
    std::fclose(f);
    const long page = sysconf(_SC_PAGESIZE);
    if (read != 2 || page <= 0) return std::nullopt;
    return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(page);
#else
    return std::nullopt;
#endif
}

} // namespace

struct RemoteBridge::Impl {
    Impl(RemoteBridgeTargets targets, std::uint32_t frames_in_flight)
        : t(std::move(targets)), shots(frames_in_flight) {}

    RemoteBridgeTargets t;

    // Declared first so responders below are destroyed (answering "dropped")
    // while the server still exists.
    std::optional<remote::RpcServer> server;
    ScreenshotCapture                shots;
    bool                             warned_primitives = false;
    std::string                      last_instanced_warning;
    std::string                      last_material_warning;

    // ---- Debugging (A5) -----------------------------------------------------
    engine::core::SimClock clock;
    struct StepWaiter {
        std::uint64_t         target = 0;
        std::function<void()> done;
    };
    std::vector<StepWaiter> step_waiters;
    // Real frame times (ms) of the last k_frame_window frames.
    static constexpr std::size_t           k_frame_window = 240;
    std::array<float, k_frame_window>      frame_ms{};
    std::size_t                            frame_count = 0; // samples stored (<= window)
    std::size_t                            frame_next = 0;
    float                                  frame_last_ms = 0.0F;
    std::optional<std::uint32_t>           draws;

    // ---- Helpers ------------------------------------------------------------

    // Runs `fn` with a transient upload context on the graphics queue (mip
    // blits need a graphics-capable queue), like the editor's load paths.
    std::expected<void, Error> with_upload(const std::function<void(const engine::rhi::TransferContext&)>& fn) {
        const VkDevice dev = t.device->handle();
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = t.device->queue_families().graphics;
        if (vkCreateCommandPool(dev, &info, nullptr, &pool) != VK_SUCCESS) {
            return fail("could not create an upload command pool");
        }
        fn(engine::rhi::TransferContext{dev, pool, t.device->graphics_queue(),
                                        t.device->max_sampler_anisotropy()});
        vkDestroyCommandPool(dev, pool, nullptr);
        return {};
    }

    [[nodiscard]] bool is_playing() const {
#ifdef ENGINE_EDITOR
        return t.play != nullptr && *t.play;
#else
        return false;
#endif
    }

    // ---- WorldServices ----------------------------------------------------------

    [[nodiscard]] remote::CameraPose camera_get() const {
        remote::CameraPose p;
        p.position = *t.cam_pos;
        p.yaw_deg = glm::degrees(*t.cam_yaw);
        p.pitch_deg = glm::degrees(*t.cam_pitch);
        p.fov_deg = glm::degrees(*t.cam_fov_y);
        return p;
    }

    void camera_set(const remote::CameraPose& p) {
        *t.cam_pos = p.position;
        *t.cam_yaw = glm::radians(p.yaw_deg);
        *t.cam_pitch = glm::radians(glm::clamp(p.pitch_deg, -89.0F, 89.0F));
        *t.cam_fov_y = glm::radians(p.fov_deg);
        if (t.reset_temporal) t.reset_temporal(); // a teleport, not a camera move
#ifndef ENGINE_EDITOR
        // Without the editor the view renders through the scene camera.
        entt::registry& r = t.scene->registry();
        if (const entt::entity cam = engine::scene::find_active_camera(r); cam != entt::null) {
            r.get<engine::scene::Camera>(cam).fov_y = *t.cam_fov_y;
        }
#endif
    }

    std::expected<std::vector<entt::entity>, Error> spawn_model(const std::filesystem::path& path) {
        std::expected<std::vector<entt::entity>, Error> roots = fail("not loaded");
        auto ctx = with_upload([&](const engine::rhi::TransferContext& transfer) {
            roots = engine::scene::GltfLoader::instantiate(path, *t.allocator, transfer, *t.assets,
                                                           *t.scene);
        });
        if (!ctx) return std::unexpected(ctx.error());
        if (roots) std::fprintf(stderr, "[remote] spawned %s\n", path.string().c_str());
        return roots;
    }

    std::expected<void, Error> load_scene(const std::filesystem::path& path) {
        if (is_playing()) return fail("stop play mode first (sim.stop)");
        std::expected<void, Error> loaded = fail("not loaded");
        auto ctx = with_upload([&](const engine::rhi::TransferContext& transfer) {
            loaded = engine::scene::load_scene(*t.scene, path, *t.allocator, transfer, *t.assets,
                                               t.render_settings);
        });
        if (!ctx) return ctx;
        if (!loaded) return loaded;
        // Rebind the loop's cached entities to the new scene.
        if (t.spin_entity != nullptr) *t.spin_entity = entt::null;
#ifdef ENGINE_EDITOR
        if (t.editor != nullptr) t.editor->set_selected(entt::null);
#endif
        if (t.reset_temporal) t.reset_temporal();
        std::fprintf(stderr, "[remote] loaded scene %s\n", path.string().c_str());
        return {};
    }

    std::expected<std::size_t, Error> nav_bake() {
        const engine::terrain::TerrainStreamer* streamer =
            t.terrain_streamer != nullptr && t.terrain_streamer->has_value()
                ? &**t.terrain_streamer
                : nullptr;
        auto built = bake_navmesh(t.scene->registry(), streamer, t.terrain_heightmap,
                                  t.terrain_anchor != nullptr ? *t.terrain_anchor : glm::vec3(0.0F));
        if (!built) return std::unexpected(built.error());
        *t.navmesh = std::move(*built);
        return t.navmesh->polygon_count();
    }

    [[nodiscard]] remote::FrameStats frame_stats() const {
        remote::FrameStats fs;
        fs.frame = *t.frame_counter;
        fs.samples = static_cast<std::uint32_t>(frame_count);
        fs.last_ms = frame_last_ms;
        float sum = 0.0F;
        for (std::size_t i = 0; i < frame_count; ++i) {
            sum += frame_ms[i];
            fs.worst_ms = std::max(fs.worst_ms, frame_ms[i]);
        }
        fs.avg_ms = frame_count > 0 ? sum / static_cast<float>(frame_count) : 0.0F;
        fs.draws = draws;
        fs.resident_bytes = resident_bytes();
        return fs;
    }

    [[nodiscard]] const engine::physics::PhysicsWorld* physics_world() const {
        for (const auto* w : {t.demo_physics, t.play_physics}) {
            if (w != nullptr && w->has_value()) return &**w;
        }
        return nullptr;
    }

    // Answers debug.step requests whose steps have run (or were dropped by
    // resume).
    void finish_steps() {
        if (step_waiters.empty()) return;
        const engine::core::SimClockState s = clock.state();
        std::vector<StepWaiter> ready;
        for (auto it = step_waiters.begin(); it != step_waiters.end();) {
            if (s.steps_run >= it->target || s.pending_steps == 0) {
                ready.push_back(std::move(*it));
                it = step_waiters.erase(it);
            } else {
                ++it;
            }
        }
        for (StepWaiter& w : ready) w.done();
    }

    remote::WorldServices services() {
        remote::WorldServices s;
        s.project_root = [this] { return t.project_root; };
        s.camera_get = [this] { return camera_get(); };
        s.camera_set = [this](const remote::CameraPose& p) { camera_set(p); };
        s.render_settings = t.render_settings;
#ifdef ENGINE_EDITOR
        if (t.play != nullptr) {
            s.playing = [this] { return *t.play; };
            s.set_playing = [this](bool play) -> std::expected<bool, Error> {
                *t.play = play; // the frame loop runs the transition right after poll()
                return play;
            };
        }
#endif
        if (t.ground_height != nullptr) {
            s.ground_height = [this](float x, float z) { return (*t.ground_height)(x, z); };
        }
        s.spawn_model = [this](const std::filesystem::path& p) { return spawn_model(p); };
        s.screenshot = [this](std::uint32_t max_size, remote::Responder r) {
            shots.request(max_size, std::move(r));
        };
        s.save_scene = [this](const std::filesystem::path& p) -> std::expected<void, Error> {
            if (is_playing()) return fail("stop play mode first (sim.stop)");
            return engine::scene::save_scene(*t.scene, p, t.render_settings);
        };
        s.load_scene = [this](const std::filesystem::path& p) { return load_scene(p); };
        if (t.navmesh != nullptr) {
            s.nav_bake = [this] { return nav_bake(); };
            s.navmesh = [this]() -> const engine::nav::NavMesh* { return t.navmesh; };
        }
        s.log_ring = &engine::core::log_ring();
        s.sim_clock = &clock;
        s.when_stepped = [this](std::uint64_t target, std::function<void()> done) {
            step_waiters.push_back({target, std::move(done)});
        };
        s.frame_stats = [this] { return frame_stats(); };
        s.physics_world = [this] { return physics_world(); };
        return s;
    }

    // ---- Endpoint ---------------------------------------------------------------

    void open_endpoint() {
        const char* env = std::getenv("QUAT_REMOTE");
        if (env == nullptr) return;
        remote::RpcServerConfig config;
        const unsigned long requested = std::strtoul(env, nullptr, 10);
        config.port = (requested > 1 && requested <= 65535) ? static_cast<std::uint16_t>(requested)
                                                            : std::uint16_t{7878};
        if (const char* token = std::getenv("QUAT_REMOTE_TOKEN"); token != nullptr) {
            config.token = token;
        }
        const bool has_token = !config.token.empty();
        auto listened = remote::RpcServer::listen(std::move(config));
        if (!listened) {
            std::fprintf(stderr, "[warn] remote control disabled: %s\n",
                         listened.error().message.c_str());
            return;
        }
        server = std::move(*listened);
        std::fprintf(stderr, "[remote] listening on 127.0.0.1:%u (%s)\n", server->port(),
                     has_token ? "token required" : "no token");

        server->add_method(
            "engine.info", "Engine name, frame counter, entity count and play state.",
            [this](const Json&) -> remote::RpcResult {
#ifdef ENGINE_EDITOR
                constexpr bool editor = true;
#else
                constexpr bool editor = false;
#endif
                return Json{{"engine", "quat-3d"},
                            {"frame", *t.frame_counter},
                            {"entities",
                             t.scene->registry().view<engine::scene::Transform>().size()},
                            {"playing", is_playing()},
                            {"paused", clock.state().paused},
                            {"editor", editor}};
            });
        // The server lives in this Impl (stable address), as batching requires.
        remote::register_world_commands(*server, *t.scene, services());
    }

    // ---- Per-frame work -----------------------------------------------------------

    void resolve_primitives() {
        if (!engine::scene::primitives_need_resolve(t.scene->registry())) return;
        std::expected<std::size_t, Error> result = fail("not run");
        auto ctx = with_upload([&](const engine::rhi::TransferContext& transfer) {
            result = engine::scene::resolve_primitives(t.scene->registry(), *t.allocator, transfer,
                                                       *t.assets);
        });
        if (!ctx || !result) {
            if (!warned_primitives) {
                std::fprintf(stderr, "[warn] primitive resolve: %s\n",
                             (!ctx ? ctx.error() : result.error()).message.c_str());
            }
            warned_primitives = true;
        }
    }

    // A8: derived materials of changed MaterialOverrides (after the
    // primitives, whose rebuilt materials are the overrides' bases). Runs
    // before the frame is recorded: replaced materials are retired as of this
    // frame and freed once the frames in flight before it completed.
    void resolve_material_overrides() {
        if (t.material_overrides == nullptr) return;
        engine::scene::MaterialOverrideStore& store = *t.material_overrides;
        const std::uint64_t frame = *t.frame_counter;
        store.collect(frame);
        if (!store.needs_resolve(t.scene->registry())) return;
        std::expected<std::size_t, Error> result = fail("not run");
        auto ctx = with_upload([&](const engine::rhi::TransferContext& transfer) {
            result = store.resolve(t.scene->registry(), *t.allocator, transfer, frame);
        });
        if (!ctx || !result) {
            const std::string& msg = (!ctx ? ctx.error() : result.error()).message;
            if (msg != last_material_warning) {
                std::fprintf(stderr, "[warn] %s\n", msg.c_str());
                last_material_warning = msg;
            }
        }
    }

    // The ground scatters stand on: the streamed tiles or the single editing
    // tile of the (first) Terrain entity; none (flat ground) without terrain.
    // While a tile is still generating the ground is known to exist but not
    // where, so placements wait for it.
    [[nodiscard]] engine::scene::ScatterGround scatter_ground() const {
        const entt::registry& r = t.scene->registry();
        const auto terrains = r.view<const engine::scene::Terrain>();
        if (terrains.begin() == terrains.end()) return {};
        const engine::scene::Terrain& terrain = terrains.get<const engine::scene::Terrain>(
            *terrains.begin());
        const glm::vec3 anchor = t.terrain_anchor != nullptr ? *t.terrain_anchor : glm::vec3(0.0F);
        std::vector<engine::scene::GroundTile> tiles;
        if (terrain.streaming) {
            if (t.terrain_streamer != nullptr && t.terrain_streamer->has_value()) {
                const engine::terrain::TerrainStreamer& s = **t.terrain_streamer;
                for (const auto& [coord, map] : s.maps()) {
                    tiles.push_back({&map, s.tile_origin(coord, anchor)});
                }
            }
        } else if (t.terrain_heightmap != nullptr && t.terrain_heightmap->valid()) {
            const float half = t.terrain_heightmap->tile_size_m * 0.5F;
            tiles.push_back({t.terrain_heightmap, anchor - glm::vec3(half, 0.0F, half)});
        }
        return engine::scene::make_tile_ground(std::move(tiles));
    }

    // R9.2: regenerate changed scatters, then (re)build the models of
    // instanced meshes whose source changed — opening an upload context
    // only when one did.
    void update_instanced_meshes() {
        entt::registry& r = t.scene->registry();
        const auto scatters = r.view<engine::scene::Scatter>();
        if (scatters.begin() != scatters.end()) {
            (void)engine::scene::scatter_system(r, scatter_ground());
        }
        if (!engine::scene::instanced_meshes_need_resolve(r)) return;
        std::expected<std::size_t, Error> result = fail("not run");
        auto ctx = with_upload([&](const engine::rhi::TransferContext& transfer) {
            result = engine::scene::resolve_instanced_meshes(r, *t.allocator, transfer, *t.assets,
                                                             t.project_root);
        });
        if (!ctx || !result) {
            // Once per failure message: a failed source is not retried until
            // it changes, but a different failure is worth reporting.
            const std::string& msg = (!ctx ? ctx.error() : result.error()).message;
            if (msg != last_instanced_warning) {
                std::fprintf(stderr, "[warn] %s\n", msg.c_str());
                last_instanced_warning = msg;
            }
        }
    }
};

RemoteBridge::RemoteBridge(RemoteBridgeTargets targets, std::uint32_t frames_in_flight)
    : impl_(std::make_unique<Impl>(std::move(targets), frames_in_flight)) {
    impl_->open_endpoint();
}
RemoteBridge::RemoteBridge(RemoteBridge&&) noexcept = default;
RemoteBridge& RemoteBridge::operator=(RemoteBridge&&) noexcept = default;
RemoteBridge::~RemoteBridge() = default;

void RemoteBridge::poll() {
    impl_->finish_steps(); // steps run last frame: reply before new requests
    if (impl_->server) impl_->server->poll();
    impl_->resolve_primitives();
    impl_->resolve_material_overrides();
    impl_->update_instanced_meshes();
    impl_->shots.finish_encodings();
}

float RemoteBridge::advance_sim(float real_dt) {
    Impl& m = *impl_;
    const float ms = std::isfinite(real_dt) && real_dt > 0.0F ? real_dt * 1000.0F : 0.0F;
    m.frame_last_ms = ms;
    m.frame_ms[m.frame_next] = ms;
    m.frame_next = (m.frame_next + 1) % Impl::k_frame_window;
    m.frame_count = std::min(m.frame_count + 1, Impl::k_frame_window);
    return m.clock.advance(real_dt);
}

float RemoteBridge::advance_sim_unrendered() { return impl_->clock.advance(0.0F); }

void RemoteBridge::note_draws(std::size_t draws) {
    impl_->draws = static_cast<std::uint32_t>(std::min<std::size_t>(draws, UINT32_MAX));
}

engine::core::StderrCapture capture_stderr_for_remote() {
    if (std::getenv("QUAT_REMOTE") == nullptr) return {};
    if (const char* opt = std::getenv("QUAT_LOG_CAPTURE"); opt != nullptr && std::string(opt) == "0") {
        return {};
    }
    auto capture = engine::core::StderrCapture::start(engine::core::log_ring());
    if (!capture) {
        engine::core::log(engine::core::LogLevel::warn, "remote", "stderr capture disabled: {}",
                          capture.error().message);
        return {};
    }
    return std::move(*capture);
}

void RemoteBridge::record_capture(VkCommandBuffer cmd, std::uint32_t frame_slot,
                                  const CaptureSource& source) {
    impl_->shots.record(cmd, frame_slot, source, *impl_->t.allocator);
}

void RemoteBridge::on_frame_complete(std::uint32_t frame_slot) {
    impl_->shots.on_frame_complete(frame_slot);
}

bool RemoteBridge::listening() const { return impl_->server.has_value(); }

} // namespace quat::app
