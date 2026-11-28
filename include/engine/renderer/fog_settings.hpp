#pragma once

// Volumetric fog settings (ROADMAP R13.3): the global exponential height
// fog of the froxel volume (FogPass) and the analytic fog beyond it; local
// fog volumes are scene components. Physical coefficients: extinction in
// 1/m (meteorological visibility ~ 3.9 / extinction).

namespace engine::renderer {

struct FogSettings {
    bool  enabled        = true;    // froxel fog (local volumes + height fog)
    float density        = 0.0F;    // extinction /m at and below the base height (0 = none)
    float height_falloff = 0.02F;   // /m: density e-folds every 1/falloff metres above the base
    float base_height_m  = 0.0F;    // world height below which the density is constant
    float albedo         = 0.9F;    // scattering / extinction
    float anisotropy     = 0.6F;    // Henyey-Greenstein g (forward scattering > 0)
    float max_distance_m = 1000.0F; // froxel grid range; the analytic fog continues beyond
};

} // namespace engine::renderer
