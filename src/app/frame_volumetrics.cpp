// FrameRenderer, per frame: the R13.3 / R13.4 volumetrics — aerial
// perspective, volumetric clouds and froxel fog (VolumetricsRuntime) —
// composited over the lit image before the transparents.

#include "app/frame_renderer.hpp"

#include <cstdio>

#include "engine/scene/scene.hpp"

namespace quat::app {

void FrameRenderer::add_volumetrics(engine::rhi::RenderGraph& graph, const FrameScene& fs,
                                    const engine::scene::CameraMatrices& cam,
                                    const engine::renderer::GBufferTargets& gbuffer,
                                    engine::rhi::ResourceHandle hdr,
                                    const engine::renderer::CascadedShadows& shadows,
                                    const engine::renderer::CloudShadowMap& cloud_shadow,
                                    const engine::renderer::ClusteredLights& lights,
                                    const engine::renderer::IblViewSet& ibl,
                                    const entt::registry* registry) {
    if (!volumetrics_) return;
    VolumetricsFrame v;
    v.hdr = hdr;
    v.depth = gbuffer.depth;
    v.render_extent = fs.render_extent;
    v.view_proj = cam.view_proj;
    v.jittered_inv_vp = fs.jittered_inv_vp;
    v.camera = cam.position;
    v.frame_number = fs.frame_number;
    v.reset = fs.reset_history;
    // The LUT sky only (sky mode 2): an HDRI has its own atmosphere baked in.
    v.atmosphere = atmosphere_ && !hdri_active() ? &*atmosphere_ : nullptr;
    v.sun_to = glm::vec3(fs.light.direction);
    v.sun_radiance = glm::vec3(fs.light.color) * fs.light.color.w;
    v.ambient = glm::vec3(fs.light.ambient);
    v.irradiance = ibl.irradiance;
    v.ibl_intensity = ibl.valid() ? settings_.ibl.intensity : 0.0F;
    v.shadows = &shadows;
    v.cloud_shadow = &cloud_shadow;
    v.lights = &lights;
    v.fog = &settings_.fog;
    v.registry = registry;
    if (auto r = volumetrics_->add_to_graph(graph, frame_, v, settings_.clouds.march_steps); !r) {
        std::fprintf(stderr, "[warn] volumetrics: %s\n", r.error().message.c_str());
    }
}

} // namespace quat::app
