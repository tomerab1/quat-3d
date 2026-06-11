#pragma once

// Volumetric cloud layer parameters (Phase 11.2). One authoritative POD shared
// by everything that renders the sky: the lighting pass (background) and the
// IBL baker (environment) pack it into two push-constant float4s that
// lib/atmosphere.slang's unpack_cloud_params() decodes, so the editor's
// Renderer panel can drive the clouds without touching shaders. Defaults match
// the original hand-tuned look.

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
    // Accumulated engine time driving the drift — set by the frame loop, not a
    // user parameter.
    float time_s = 0.0F;

    // The two push-constant words consumed by unpack_cloud_params(). A zero
    // coverage in .first disables the layer in the shader.
    [[nodiscard]] glm::vec4 pack_a() const {
        return {enabled ? coverage : 0.0F, density, bottom_km, thickness_km};
    }
    [[nodiscard]] glm::vec4 pack_b() const {
        const float heading = glm::radians(wind_dir_deg);
        const float drift_km = wind_speed * time_s * 0.001F;
        return {1.0F / std::max(size_km, 0.5F), detail, drift_km * std::cos(heading),
                drift_km * std::sin(heading)};
    }
};

} // namespace engine::renderer
