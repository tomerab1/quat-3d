#include "app/grass_runtime.hpp"

#include <cstdio>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/renderer/terrain_pass.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

std::optional<engine::renderer::GrassSettings> active_grass(const entt::registry& registry) {
    for (const auto [e, grass] : registry.view<const engine::scene::Grass>().each()) {
        return grass.settings;
    }
    return std::nullopt;
}

GrassRuntime::GrassRuntime(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                           engine::rhi::PipelineCache& cache,
                           const engine::rhi::TransferContext& transfer, std::string shader_dir,
                           std::uint32_t frames_in_flight)
    : device_(device), allocator_(allocator), cache_(cache), transfer_(transfer),
      shader_dir_(std::move(shader_dir)), frames_in_flight_(frames_in_flight) {}

void GrassRuntime::add_to_graph(engine::rhi::RenderGraph& graph, std::uint32_t slot,
                                const entt::registry& registry,
                                const engine::renderer::GBufferTargets& gbuffer, VkExtent2D extent,
                                const glm::mat4& draw_view_proj,
                                const engine::scene::CameraMatrices& camera,
                                const engine::renderer::TerrainPass* terrain,
                                const glm::vec3& terrain_anchor, float dt,
                                const engine::renderer::WindFieldOutput& wind) {
    const double prev_wind_time_s = wind_time_s_;
    wind_time_s_ += static_cast<double>(dt);
    const auto settings = active_grass(registry);
    if (!settings || create_failed_) return;
    if (!pass_) {
        auto created = engine::renderer::GrassPass::create(device_, allocator_, cache_, transfer_,
                                                           shader_dir_, frames_in_flight_);
        if (!created) {
            create_failed_ = true;
            std::fprintf(stderr, "[warn] grass disabled: %s\n", created.error().message.c_str());
            return;
        }
        pass_ = std::move(*created);
    }

    engine::renderer::GrassFrame frame;
    frame.settings = *settings;
    frame.draw_view_proj = draw_view_proj;
    frame.cull_view_proj = camera.view_proj;
    frame.camera = camera.position;
    frame.time_s = static_cast<float>(wind_time_s_);
    frame.prev_time_s = static_cast<float>(prev_wind_time_s); // velocity of the wind motion
    frame.wind = wind;
    frame.pixels_per_metre = engine::renderer::grass_pixels_per_metre(camera.projection, extent.height);
    std::vector<engine::renderer::TerrainHeightTile> tiles;
    if (terrain != nullptr) {
        // 1.5 x radius: the ground cover fades out by then (grass_cover_fade),
        // so a tile entering that band is already bound and the fade's end
        // does not hop when it starts to count. Terrain tiles are hundreds of
        // metres, so four tiles cover the disc.
        tiles = terrain->height_tiles(glm::vec2(camera.position.x, camera.position.z),
                                      1.5F * frame.settings.radius, terrain_anchor,
                                      engine::renderer::grass_max_height_tiles);
    }
    frame.terrain = tiles;
    if (auto r = pass_->add_to_graph(graph, slot, frame, &gbuffer, extent); !r) {
        if (r.error().message != last_error_) {
            last_error_ = r.error().message;
            std::fprintf(stderr, "[warn] grass: %s\n", last_error_.c_str());
        }
    } else {
        last_error_.clear();
    }
}

} // namespace quat::app
