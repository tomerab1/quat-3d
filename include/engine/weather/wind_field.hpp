#pragma once

// The global wind field (ROADMAP R13.1) — CPU reference.
//
// The wind at a point is the weather's mean wind, modulated by gust fronts and
// stirred by turbulence, all advected downwind with the mean flow (Taylor's
// frozen-turbulence hypothesis: eddies travel with the wind and slowly
// decorrelate):
//
//   gust fronts  two octaves of value noise on a lattice stretched across the
//                wind (k_gust_along_m x k_gust_across_m cells), so the strong
//                bands lie perpendicular to the flow and sweep downwind;
//   turbulence   a 3D value-noise vector (two horizontal components and a
//                vertical one) on a k_turbulence_m lattice whose third axis is
//                time, so the swirl evolves while it drifts.
//
// The field is evaluated at the k_wind_reference_height_m reference height;
// wind_profile() scales it to another height above the ground with the
// neutral-stability logarithmic wind profile. The log law only holds in the
// surface layer, so the profile stops at k_surface_layer_top_m; the free
// atmosphere above (cloud drift) uses the fixed k_geostrophic_factor, the
// typical ratio of the geostrophic wind to the 10 m wind over land.
//
// The renderer's wind_field.slang evaluates exactly this function on the GPU
// (WindFieldPass writes it into a texture around the camera that grass
// samples); tests compare the two. To keep float precision independent of how
// long the wind has blown, the advection offsets are wrapped on the CPU to the
// noise lattice's period (k_wind_lattice_period cells — the lattice hashes are
// periodic, so wrapping is seamless) and both sides evaluate the same float
// operations from the same constants (no division on the GPU).

#include <cstdint>

#include <glm/glm.hpp>

namespace engine::weather {

struct WeatherState;

inline constexpr float k_wind_reference_height_m = 10.0F;
inline constexpr float k_wind_roughness_length_m = 0.03F; // open grassland
inline constexpr float k_surface_layer_top_m = 150.0F;     // log law's upper limit
inline constexpr float k_geostrophic_factor = 1.6F;        // free-atmosphere wind / 10 m wind
inline constexpr float k_gust_along_m = 48.0F;   // gust lattice cell along the wind
inline constexpr float k_gust_across_m = 160.0F; // ... and across it
inline constexpr float k_turbulence_m = 14.0F;   // swirl lattice cell
inline constexpr float k_turbulence_evolve_s = 6.0F; // swirl decorrelation time
inline constexpr std::uint32_t k_wind_lattice_period = 1024; // cells (power of two)

// How far the air has travelled and for how long: the wind field's clock.
// The frame loop integrates it (advance()) so that changing the wind speed
// does not jump the field; the reference wind_at() uses speed * time.
struct WindAdvection {
    double distance_m = 0.0; // integral of the mean wind speed over time
    double time_s = 0.0;

    void advance(float speed_m_s, float dt) {
        distance_m += static_cast<double>(speed_m_s) * static_cast<double>(dt);
        time_s += static_cast<double>(dt);
    }
};

// One frame's field constants: the weather and its clock folded into what the
// CPU reference and the GPU both evaluate.
struct WindFieldParams {
    glm::vec2 direction{1.0F, 0.0F}; // unit heading the wind blows toward (XZ)
    float     speed = 0.0F;          // mean m/s at the reference height
    float     gustiness = 0.0F;
    float     turbulence = 0.0F;
    float     gust_offset = 0.0F;    // gust lattice offset along the wind (wrapped)
    float     swirl_offset = 0.0F;   // swirl lattice offset along the wind (wrapped)
    float     swirl_phase = 0.0F;    // swirl lattice time coordinate (wrapped)
};

[[nodiscard]] WindFieldParams wind_field_params(const WeatherState& state,
                                                const WindAdvection& clock);

struct WindSample {
    glm::vec3 velocity{0.0F}; // m/s, world space (y up)
    float     gust = 1.0F;    // gust multiplier on the mean wind (1 = mean)
};

// The field at world position (x, z), at the reference height.
[[nodiscard]] WindSample sample_wind(const WindFieldParams& params, glm::vec2 xz);

// Wind velocity (m/s) at `position` after `time_s` seconds of a steady
// `state`, at the reference height (position.y is not used: scale with
// wind_profile() for a height above the ground). The query physics, audio and
// tests use.
[[nodiscard]] glm::vec3 wind_at(const WeatherState& state, const glm::vec3& position,
                                double time_s);

// Logarithmic wind profile: the factor turning the reference-height wind into
// the wind `height_above_ground_m` above the ground (1 at the reference
// height). Heights are clamped to [1.5 roughness lengths,
// k_surface_layer_top_m]: above the surface layer the log law no longer
// holds (use k_geostrophic_factor for the free atmosphere).
[[nodiscard]] float wind_profile(float height_above_ground_m);

} // namespace engine::weather
