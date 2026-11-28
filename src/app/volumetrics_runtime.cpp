#include "app/volumetrics_runtime.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/astro/sky_state.hpp"
#include "engine/core/profile.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/fog_volume.hpp"

namespace quat::app {

namespace {

// Extra-atmospheric solar illuminance at 1 au: the sky-view LUT's unit
// irradiance (R22.1 SkyState reports the Sun and Moon in lux).
constexpr float k_sun_reference_lux = 128000.0F;
// lighting.slang SKY_LUT_BRIGHTNESS: HDR units per unit of LUT luminance.
constexpr float k_sky_lut_scale = 8.0F;

[[nodiscard]] std::vector<engine::renderer::FogVolumeGpu>
gather_fog_volumes(const entt::registry* registry) {
    std::vector<engine::renderer::FogVolumeGpu> out;
    if (registry == nullptr) return out;
    for (const auto [e, t, v] :
         registry->view<const engine::scene::Transform, const engine::scene::FogVolume>().each()) {
        if (out.size() >= engine::renderer::fog_max_volumes) break;
        if (!(v.density > 0.0F) || glm::abs(glm::determinant(glm::mat3(t.world))) < 1e-9F) {
            continue;
        }
        out.push_back(engine::renderer::make_fog_volume(
            t.world, v.shape == engine::scene::FogVolumeShape::ellipsoid, v.density, v.albedo,
            v.edge_fade, v.height_falloff));
    }
    return out;
}

} // namespace

std::expected<std::unique_ptr<VolumetricsRuntime>, engine::core::Error>
VolumetricsRuntime::create(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                           engine::rhi::PipelineCache& cache, const std::string& shader_dir) {
    QUAT_ZONE_N("volumetrics_create");
    auto out = std::make_unique<VolumetricsRuntime>();
    auto noise = engine::renderer::CloudNoisePass::create(device, allocator, cache, shader_dir);
    if (!noise) return std::unexpected(noise.error());
    out->noise_ = std::move(*noise);
    out->cloud_history_ =
        std::make_shared<engine::renderer::CloudHistory>(device, allocator, k_slots);
    out->fog_history_ = std::make_shared<engine::renderer::FogHistory>(device, allocator);
    for (Slot& s : out->slots_) {
        auto aerial = engine::renderer::AerialPerspectivePass::create(device, allocator, cache,
                                                                      shader_dir);
        if (!aerial) return std::unexpected(aerial.error());
        s.aerial = std::move(*aerial);
        auto clouds = engine::renderer::VolumetricCloudPass::create(device, allocator, cache,
                                                                    shader_dir, out->cloud_history_);
        if (!clouds) return std::unexpected(clouds.error());
        s.clouds = std::move(*clouds);
        auto fog = engine::renderer::FogPass::create(device, allocator, cache, shader_dir,
                                                     out->fog_history_);
        if (!fog) return std::unexpected(fog.error());
        s.fog = std::move(*fog);
        auto composite =
            engine::renderer::AtmosphericsPass::create(device, allocator, cache, shader_dir);
        if (!composite) return std::unexpected(composite.error());
        s.composite = std::move(*composite);
    }
    return out;
}

void VolumetricsRuntime::begin_frame(const engine::renderer::CloudSettings& clouds,
                                     bool world_anchored, const glm::vec3& camera,
                                     const glm::vec3& sun_to, const entt::registry* registry) {
    engine::renderer::CloudLights lights;
    lights.sun_to = sun_to;
    lights.sun_irradiance = 1.0F;
    if (registry != nullptr) {
        for (const auto [e, sky] : registry->view<const engine::astro::SkyState>().each()) {
            // The Moon's extra-atmospheric illuminance relative to the
            // reference Sun; the atmosphere's transmittance applies in the
            // shaders. Below the horizon the planet blocks it there too.
            if (sky.moon_illuminance_lux > 0.0F && sky.moon_disc_visible > 0.0F) {
                lights.moon_to = sky.moon_direction;
                lights.moon_irradiance = sky.moon_illuminance_lux / k_sun_reference_lux;
            }
            if (sky.sun_illuminance_lux > 0.0F) {
                lights.sun_irradiance = sky.sun_illuminance_lux / k_sun_reference_lux;
            }
            break;
        }
    }
    lights_ = lights;
    layer_ = engine::renderer::make_cloud_layer(clouds, world_anchored, camera, lights);
}

std::expected<void, engine::core::Error>
VolumetricsRuntime::add_to_graph(engine::rhi::RenderGraph& graph, std::uint32_t slot,
                                 const VolumetricsFrame& in, int march_steps) {
    QUAT_ZONE_N("volumetrics");
    if (slot >= k_slots) return std::unexpected(engine::core::Error{"volumetrics: bad slot"});
    Slot& s = slots_[slot];
    const glm::mat4 inv_vp = glm::inverse(in.view_proj);
    const bool reset = in.reset || !has_prev_;
    const glm::mat4 prev_vp = reset ? in.view_proj : prev_view_proj_;
    const glm::vec2 camera_km = glm::vec2(in.camera.x, in.camera.z) * 0.001F;
    const glm::vec2 anchor_km(layer_.offset.x, layer_.offset.y);
    const glm::vec2 drift_now = camera_km - anchor_km;
    const glm::vec2 drift_prev =
        has_prev_ ? glm::vec2(prev_camera_.x, prev_camera_.z) * 0.001F - prev_anchor_km_ : drift_now;
    const glm::vec3 prev_camera = reset ? in.camera : prev_camera_;
    prev_view_proj_ = in.view_proj;
    prev_camera_ = in.camera;
    prev_anchor_km_ = anchor_km;
    has_prev_ = true;

    engine::renderer::AtmosphericsInputs comp;
    comp.hdr = in.hdr;
    comp.depth = in.depth;
    comp.extent = in.render_extent;
    comp.inv_view_proj = in.jittered_inv_vp;
    comp.camera = in.camera;
    comp.sky_scale = k_sky_lut_scale;
    comp.irradiance = in.irradiance;

    // Aerial perspective and clouds need the LUT atmosphere.
    if (in.atmosphere != nullptr && in.atmosphere->valid()) {
        engine::renderer::AerialPerspectiveInputs ap;
        ap.inv_view_proj = inv_vp;
        ap.sun_to = lights_.sun_to;
        ap.sun_irradiance = lights_.sun_irradiance;
        ap.moon_to = lights_.moon_to;
        ap.moon_irradiance = lights_.moon_irradiance;
        ap.transmittance = in.atmosphere->transmittance_view();
        ap.multiscatter = in.atmosphere->multiscatter_view();
        auto volume = s.aerial.add_to_graph(graph, ap);
        if (!volume) return std::unexpected(volume.error());
        comp.aerial_perspective = *volume;
        if (layer_.shell.z > 0.0F || layer_.cirrus.x > 0.0F) {
            engine::renderer::VolumetricCloudInputs ci;
            ci.layer = layer_;
            ci.inv_view_proj = inv_vp;
            ci.prev_view_proj = prev_vp;
            ci.camera = in.camera;
            ci.drift_m = (drift_now - drift_prev) * 1000.0F;
            ci.frame_index = in.frame_number;
            ci.reset = reset;
            ci.march_steps = march_steps;
            ci.depth = in.depth;
            ci.render_extent = in.render_extent;
            ci.aerial_perspective = *volume;
            ci.transmittance = in.atmosphere->transmittance_view();
            ci.skyview = in.atmosphere->skyview_view();
            auto clouds = s.clouds.add_to_graph(graph, ci, noise_);
            if (!clouds) return std::unexpected(clouds.error());
            comp.clouds = *clouds;
        }
    }

    // Fog: the height fog and every FogVolume, lit like the scene.
    const engine::renderer::FogSettings fog =
        in.fog != nullptr ? *in.fog : engine::renderer::FogSettings{};
    engine::renderer::FogMedia media;
    media.extinction = fog.density;
    media.height_falloff = fog.height_falloff;
    media.base_height_m = fog.base_height_m;
    media.anisotropy = fog.anisotropy;
    media.albedo = glm::vec3(fog.albedo);
    media.max_distance_m = fog.max_distance_m;
    media.sun_to = in.sun_to;
    media.sun_radiance = in.sun_radiance;
    media.ambient = in.ambient;
    media.ibl_intensity = in.ibl_intensity;
    comp.media = media;
    comp.height_fog = fog.enabled && fog.density > 0.0F;
    std::vector<engine::renderer::FogVolumeGpu> volumes = gather_fog_volumes(in.registry);
    if (fog.enabled && (fog.density > 0.0F || !volumes.empty())) {
        engine::renderer::FogInputs fi;
        fi.media = media;
        fi.volumes = std::move(volumes);
        fi.inv_view_proj = inv_vp;
        fi.prev_view_proj = prev_vp;
        fi.camera = in.camera;
        fi.prev_camera = prev_camera;
        fi.frame_index = in.frame_number;
        fi.reset = reset;
        fi.shadows = in.shadows;
        fi.cloud_shadow = in.cloud_shadow;
        fi.lights = in.lights;
        fi.irradiance = in.irradiance;
        auto volume = s.fog.add_to_graph(graph, fi);
        if (!volume) return std::unexpected(volume.error());
        comp.fog = *volume;
    }
    return s.composite.add_to_graph(graph, comp);
}

} // namespace quat::app
