// RemoteBridge — see remote_bridge.hpp.

#include "app/remote_bridge.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/gtc/constants.hpp>

#include "app/nav_bake.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/remote/image_encode.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/remote/world_commands.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/instanced_mesh.hpp"
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

constexpr std::size_t k_max_pending_shots = 8;

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// Which 8-bit layouts the readback understands; nullopt for anything else.
[[nodiscard]] std::optional<bool> is_bgra(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: return true;
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: return false;
    default: return std::nullopt;
    }
}

struct EncodedShot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string   base64;
};

} // namespace

struct RemoteBridge::Impl {
    RemoteBridgeTargets t;

    struct PendingShot {
        std::uint32_t     max_size = 1024;
        remote::Responder responder;
    };
    // One readback buffer per frame-in-flight slot; `waiting` are the
    // requests whose copy was recorded into that slot's last submission.
    struct Slot {
        engine::rhi::GpuBuffer   buffer;
        VkExtent2D               extent{0, 0};
        bool                     bgra = false;
        std::vector<PendingShot> waiting;
    };
    struct Encoding {
        std::future<std::expected<EncodedShot, Error>> result;
        remote::Responder                              responder;
    };

    // Declared first so responders below are destroyed (answering "dropped")
    // while the server still exists.
    std::optional<remote::RpcServer> server;
    std::vector<PendingShot>         pending;
    std::vector<Slot>                slots;
    std::vector<Encoding>            encodings;
    bool                             warned_primitives = false;
    std::string                      last_instanced_warning;

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
            loaded = engine::scene::load_scene(*t.scene, path, *t.allocator, transfer, *t.assets);
        });
        if (!ctx) return ctx;
        if (!loaded) return loaded;
        // Rebind the loop's cached entities to the new scene.
        if (t.spin_entity != nullptr) *t.spin_entity = entt::null;
#ifdef ENGINE_EDITOR
        if (t.editor != nullptr) t.editor->set_selected(entt::null);
#endif
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

    void request_screenshot(std::uint32_t max_size, remote::Responder responder) {
        std::size_t in_flight = pending.size() + encodings.size();
        for (const Slot& s : slots) in_flight += s.waiting.size();
        if (in_flight >= k_max_pending_shots) {
            responder.reply(remote::rpc_fail(remote::rpc_code::failed,
                                             "too many screenshots in flight; retry shortly"));
            return;
        }
        pending.push_back({max_size, std::move(responder)});
    }

    remote::WorldServices services() {
        remote::WorldServices s;
        s.project_root = [this] { return t.project_root; };
        s.camera_get = [this] { return camera_get(); };
        s.camera_set = [this](const remote::CameraPose& p) { camera_set(p); };
#ifdef ENGINE_EDITOR
        if (t.render_settings != nullptr) {
            s.clouds_get = [this] { return t.render_settings->clouds; };
            s.clouds_set = [this](const engine::renderer::CloudSettings& c) {
                const float time = t.render_settings->clouds.time_s;
                t.render_settings->clouds = c;
                t.render_settings->clouds.time_s = time;
            };
        }
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
            request_screenshot(max_size, std::move(r));
        };
        s.save_scene = [this](const std::filesystem::path& p) -> std::expected<void, Error> {
            if (is_playing()) return fail("stop play mode first (sim.stop)");
            return engine::scene::save_scene(*t.scene, p);
        };
        s.load_scene = [this](const std::filesystem::path& p) { return load_scene(p); };
        if (t.navmesh != nullptr) s.nav_bake = [this] { return nav_bake(); };
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

    void finish_encodings() {
        for (auto it = encodings.begin(); it != encodings.end();) {
            if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto shot = it->result.get();
            if (shot) {
                it->responder.reply(Json{{"mime", "image/png"},
                                         {"width", shot->width},
                                         {"height", shot->height},
                                         {"data", std::move(shot->base64)}});
            } else {
                it->responder.reply(remote::rpc_fail(remote::rpc_code::failed,
                                                     "screenshot: " + shot.error().message));
            }
            it = encodings.erase(it);
        }
    }

    void record_capture(VkCommandBuffer cmd, std::uint32_t slot_index, const CaptureSource& src) {
        if (pending.empty() || slot_index >= slots.size()) return;
        Slot& slot = slots[slot_index];
        if (!slot.waiting.empty()) return; // on_frame_complete not run yet: next frame
        const std::optional<bool> bgra = is_bgra(src.format);
        const auto fail_pending = [&](const std::string& why) {
            for (PendingShot& p : pending) {
                p.responder.reply(remote::rpc_fail(remote::rpc_code::failed, "screenshot: " + why));
            }
            pending.clear();
        };
        if (!bgra || src.image == VK_NULL_HANDLE || src.extent.width == 0 || src.extent.height == 0) {
            fail_pending("the frame's colour format is not 8-bit RGBA/BGRA");
            return;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(src.extent.width) * src.extent.height * 4;
        if (slot.buffer.handle() == VK_NULL_HANDLE || slot.buffer.size() < bytes) {
            slot.buffer = {};
            auto buf = t.allocator->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VMA_MEMORY_USAGE_AUTO,
                                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
            if (!buf || buf->mapped() == nullptr) {
                fail_pending("could not allocate a readback buffer");
                return;
            }
            slot.buffer = std::move(*buf);
        }

        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = src.layout;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = src.image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_src;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {src.extent.width, src.extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               slot.buffer.handle(), 1, &region);

        // Back to the layout the frame left it in (later consumers — present,
        // the next frame's import — are ordered by the submit's semaphore /
        // the slot fence), and make the copy visible to host reads.
        VkImageMemoryBarrier2 back = to_src;
        back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        back.srcAccessMask = 0;
        back.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        back.dstAccessMask = 0;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = src.layout;
        VkBufferMemoryBarrier2 to_host{};
        to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = slot.buffer.handle();
        to_host.offset = 0;
        to_host.size = bytes;
        dep.pImageMemoryBarriers = &back;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &to_host;
        vkCmdPipelineBarrier2(cmd, &dep);

        slot.extent = src.extent;
        slot.bgra = *bgra;
        slot.waiting = std::move(pending);
        pending.clear();
    }

    void on_frame_complete(std::uint32_t slot_index) {
        if (slot_index >= slots.size() || slots[slot_index].waiting.empty()) return;
        Slot& slot = slots[slot_index];
        std::vector<PendingShot> waiting = std::move(slot.waiting);
        slot.waiting.clear();
        if (auto ok = slot.buffer.invalidate_mapped(); !ok) {
            for (PendingShot& p : waiting) {
                p.responder.reply(remote::rpc_fail(remote::rpc_code::failed,
                                                   "screenshot: " + ok.error().message));
            }
            return;
        }
        // Copy out (fast) so the slot's buffer is free for the next frame;
        // resample + PNG + base64 run on a worker thread.
        const std::size_t bytes = static_cast<std::size_t>(slot.extent.width) * slot.extent.height * 4;
        const auto* mapped = static_cast<const std::uint8_t*>(slot.buffer.mapped());
        auto pixels = std::make_shared<const std::vector<std::uint8_t>>(mapped, mapped + bytes);
        const VkExtent2D extent = slot.extent;
        const bool bgra = slot.bgra;
        for (PendingShot& p : waiting) {
            Encoding enc;
            enc.responder = std::move(p.responder);
            enc.result = std::async(std::launch::async, [pixels, extent, bgra,
                                                         max_size = p.max_size]()
                                                            -> std::expected<EncodedShot, Error> {
                auto img = remote::encode_screenshot(*pixels, extent.width, extent.height,
                                                     static_cast<std::size_t>(extent.width) * 4,
                                                     bgra, max_size);
                if (!img) return std::unexpected(img.error());
                return EncodedShot{img->width, img->height, remote::base64_encode(img->png)};
            });
            encodings.push_back(std::move(enc));
        }
    }
};

RemoteBridge::RemoteBridge(RemoteBridgeTargets targets, std::uint32_t frames_in_flight)
    : impl_(std::make_unique<Impl>()) {
    impl_->t = std::move(targets);
    impl_->slots.resize(frames_in_flight);
    impl_->open_endpoint();
}
RemoteBridge::RemoteBridge(RemoteBridge&&) noexcept = default;
RemoteBridge& RemoteBridge::operator=(RemoteBridge&&) noexcept = default;
RemoteBridge::~RemoteBridge() = default;

void RemoteBridge::poll() {
    if (impl_->server) impl_->server->poll();
    impl_->resolve_primitives();
    impl_->update_instanced_meshes();
    impl_->finish_encodings();
}

void RemoteBridge::record_capture(VkCommandBuffer cmd, std::uint32_t frame_slot,
                                  const CaptureSource& source) {
    impl_->record_capture(cmd, frame_slot, source);
}

void RemoteBridge::on_frame_complete(std::uint32_t frame_slot) {
    impl_->on_frame_complete(frame_slot);
}

bool RemoteBridge::listening() const { return impl_->server.has_value(); }

} // namespace quat::app
