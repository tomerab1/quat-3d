// FrameRenderer — the night sky (ROADMAP R22.2.1).
//
// The world clock's SkyState (gathered with the scene) feeds three things:
//   - the sky-view LUT's extra lights for the planet surface below the
//     horizon (the Moon's irradiance and the night-sky floor; the Sun is the
//     LUT's own unit light),
//   - the bright star field over the lit HDR (StarFieldPass), rotated by the
//     clock's celestial matrix,
// all in the renderer's photometric units (photometry.hpp), pre-exposed with
// this frame's expected exposure (choose_pre_exposure, frame_post.cpp).

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "app/frame_renderer.hpp"
#include "engine/astro/star_catalogue.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/renderer/photometry.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

namespace r = engine::renderer;

void FrameRenderer::create_star_field(engine::rhi::PipelineCache& pipeline_cache,
                                      const std::string& shader_dir,
                                      const std::filesystem::path& catalogue) {
    auto stars = engine::astro::load_star_catalogue(engine::core::path_to_utf8(catalogue));
    if (!stars) {
        std::fprintf(stderr, "[warn] no star field: %s\n", stars.error().message.c_str());
        return;
    }
    auto gpu = r::StarCatalogueGpu::create(*device_, *allocator_, *stars);
    if (!gpu) {
        std::fprintf(stderr, "[warn] no star field: %s\n", gpu.error().message.c_str());
        return;
    }
    star_catalogue_ = std::move(*gpu);
    for (DeferredFrame& f : frames_) {
        auto pass = r::StarFieldPass::create(*device_, *allocator_, pipeline_cache, shader_dir,
                                             star_catalogue_);
        if (!pass) {
            std::fprintf(stderr, "[warn] no star field: %s\n", pass.error().message.c_str());
            for (DeferredFrame& g : frames_) g.stars = r::StarFieldPass{};
            return;
        }
        f.stars = std::move(*pass);
    }
    std::fprintf(stderr, "[info] star field: %u stars\n", star_catalogue_->count());
}

r::SkyLights FrameRenderer::sky_lights(const FrameScene& scene) const {
    r::SkyLights lights;
    // The natural night-sky floor lights the ground whatever the clock says.
    lights.night_irradiance =
        r::photometry::k_night_sky_floor_lux / r::photometry::k_sun_reference_lux;
    if (scene.sky) {
        const engine::astro::SkyState& sky = *scene.sky;
        lights.moon_direction = sky.moon_direction;
        // Extra-atmospheric illuminance; the LUT applies the transmittance.
        lights.moon_irradiance = sky.moon_illuminance_lux * sky.moon_disc_visible /
                                 r::photometry::k_sun_reference_lux;
    }
    return lights;
}

void FrameRenderer::add_star_field(engine::rhi::RenderGraph& graph, const FrameInputs& in,
                                   const r::GBufferTargets& gbuffer,
                                   engine::rhi::ResourceHandle hdr,
                                   const r::CloudSettings& clouds) {
    const FrameScene& fs = *in.scene;
    DeferredFrame& fr = frames_[frame_];
    // Stars need the clock's sky, the atmosphere (transmittance LUT; the
    // HDRI sky has its own stars or none) and the catalogue.
    if (!fs.sky || !atmosphere_ || hdri_active() || !fr.stars.valid() ||
        !settings_.night_sky.stars) {
        return;
    }
    const engine::scene::CameraMatrices& cam = *in.cam;
    r::StarFieldFrame frame;
    frame.view_proj = fs.jittered_vp;
    frame.projection = cam.projection;
    // The view matrix's third row is the camera's back axis.
    frame.camera_forward = -glm::vec3(cam.view[0][2], cam.view[1][2], cam.view[2][2]);
    frame.world_from_celestial = fs.sky->world_from_celestial;
    frame.extent = fs.render_extent;
    frame.pre_exposure = pre_exposure_;
    frame.time_s = cloud_time_s_;
    frame.scintillation = settings_.night_sky.scintillation;
    frame.sun_direction = glm::vec3(fs.light.direction);
    frame.clouds = clouds;
    if (auto r = fr.stars.add_to_graph(graph, hdr, gbuffer.depth, atmosphere_->transmittance_view(),
                                       frame);
        !r) {
        std::fprintf(stderr, "[warn] star field: %s\n", r.error().message.c_str());
    }
}

} // namespace quat::app
