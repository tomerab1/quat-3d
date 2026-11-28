#pragma once

// Volumetric cloud layer parameters (Phase 11.2; R13.4 clouds v2). One
// authoritative POD shared by everything that renders the clouds: the
// volumetric cloud pass and the cloud shadow map build their CloudLayerGpu
// from it (cloud_reference.hpp make_cloud_layer), the IBL baker packs it into
// two push-constant float4s that lib/atmosphere.slang's unpack_cloud_params()
// decodes, so the editor's Renderer panel can drive the clouds without
// touching shaders.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace engine::renderer {

struct CloudSettings {
    bool  enabled      = true;
    float coverage     = 0.42F; // sky fraction covered (0..1)
    float density      = 24.0F; // extinction /km at full density
    float bottom_km    = 1.5F;  // shell bottom altitude above the ground
    float thickness_km = 2.5F;  // shell thickness
    float size_km      = 10.0F; // base feature ("puff") size
    float detail       = 0.4F;  // edge erosion strength (0..1)
    float wind_speed   = 12.0F; // drift speed, m/s
    float wind_dir_deg = 35.0F; // world-space heading the clouds drift toward
    bool  shadows      = true;  // with weather: clouds shadow the sun (R13.1)
    // R13.4: cloud type (0 stratus, 0.5 stratocumulus, 1 cumulus; the
    // weather lowers it toward stratus as the sky closes) and the high
    // cirrus layer.
    float type               = 0.8F;
    float cirrus_coverage    = 0.35F; // 0 = none
    float cirrus_altitude_km = 9.0F;
    float cirrus_opacity     = 0.35F; // optical depth of a full cirrus sheet
    int   march_steps        = 48;    // samples of a vertical ray (more toward the horizon)
    // Accumulated engine time driving the drift — set by the frame loop, not a
    // user parameter.
    float time_s = 0.0F;
    // Extra offset of the cloud field (km, world XZ) — frame-loop state, not a
    // user parameter. 0 without weather (camera-relative clouds, as before
    // R13.1); with weather, the camera's world position (so clouds and their
    // shadows stay anchored to the world) plus the drift integrated from the
    // weather's wind (scene::frame_clouds).
    glm::vec2 offset_km{0.0F};
    // The weather's precipitation (0..1) — frame-loop state like time_s
    // (scene::frame_clouds), not a user parameter: rain clouds are
    // optically thicker and darker at the base.
    float precipitation = 0.0F;

    // The two push-constant words consumed by unpack_cloud_params(). A zero
    // coverage in .first disables the layer in the shader.
    [[nodiscard]] glm::vec4 pack_a() const {
        return {enabled ? coverage : 0.0F, density, bottom_km, thickness_km};
    }
    [[nodiscard]] glm::vec4 pack_b() const {
        const float heading = glm::radians(wind_dir_deg);
        const float drift_km = wind_speed * time_s * 0.001F;
        return {1.0F / std::max(size_km, 0.5F), detail,
                drift_km * std::cos(heading) + offset_km.x,
                drift_km * std::sin(heading) + offset_km.y};
    }
};

} // namespace engine::renderer
