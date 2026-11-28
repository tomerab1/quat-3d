#pragma once

// Tunable settings of the R21 lighting-realism features: the environment
// (procedural / atmosphere sky or an HDRI panorama, HdriEnvironment) and
// screen-space reflections (SsrPass). They are the `environment` and `ssr`
// groups of A8's RenderSettings (render_settings.hpp), whose descriptor
// table drives the editor panel, render.settings.* and scene files. The
// HDRI's file path is scene data, not a setting (environment.set hdri.path).

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include "engine/renderer/shadow_ao_settings.hpp" // FloatRange

namespace engine::renderer {

enum class SkySource : std::uint8_t {
    procedural, // the physically based atmosphere (or the procedural fallback)
    hdri,       // the loaded HDRI panorama: background, ambient and reflections
};
inline constexpr std::array<std::string_view, 2> sky_source_names{"procedural", "hdri"};

struct EnvironmentSettings {
    int   source = static_cast<int>(SkySource::procedural); // index into sky_source_names
    float hdri_intensity = 1.0F;     // multiplies the HDRI (and its sun)
    float hdri_rotation_deg = 0.0F;  // about +Y, counter-clockwise seen from above
    // The HDRI's sun becomes the scene's directional light (direction,
    // colour, intensity, cascaded shadows) and is clamped out of the maps so
    // it is not counted twice. Off: the HDRI lights as captured and the
    // scene's own DirectionalLight stays.
    bool  hdri_sun = true;

    static constexpr FloatRange k_intensity{0.0F, 100.0F};
    static constexpr FloatRange k_rotation_deg{-360.0F, 360.0F};

    // Intensity and rotation in range (the bake and the HDRI's sun use these).
    [[nodiscard]] constexpr EnvironmentSettings sanitized() const {
        EnvironmentSettings s = *this;
        s.hdri_intensity = k_intensity.clamp(hdri_intensity);
        s.hdri_rotation_deg = k_rotation_deg.clamp(hdri_rotation_deg);
        return s;
    }
};

[[nodiscard]] SkySource sky_source(const EnvironmentSettings& settings);

enum class SsrQuality : std::uint8_t { low, medium, high };
inline constexpr std::array<std::string_view, 3> ssr_quality_names{"low", "medium", "high"};

// Screen-space reflections: stochastic GGX rays traced through the Hi-Z
// pyramid, resolved with ray reuse and accumulated temporally; the
// environment fills what they miss.
struct SsrSettings {
    bool  enabled = true;
    int   quality = static_cast<int>(SsrQuality::medium); // index into ssr_quality_names
    float max_roughness = 0.6F; // no reflections above; they fade out from 75% of it
    float thickness = 0.25F;    // metres behind the depth buffer a hit still counts
    float edge_fade = 0.1F;     // fraction of the screen height faded at the edges

    static constexpr FloatRange k_max_roughness{0.0F, 1.0F};
    static constexpr FloatRange k_thickness{0.01F, 5.0F};
    static constexpr FloatRange k_edge_fade{0.0F, 0.5F};

    [[nodiscard]] constexpr SsrSettings sanitized() const {
        SsrSettings s = *this;
        s.quality = std::clamp(quality, 0, static_cast<int>(ssr_quality_names.size()) - 1);
        s.max_roughness = k_max_roughness.clamp(max_roughness);
        s.thickness = k_thickness.clamp(thickness);
        s.edge_fade = k_edge_fade.clamp(edge_fade);
        return s;
    }
};

// March steps and reused neighbour rays of a quality level.
struct SsrBudget {
    std::uint32_t max_steps = 64;
    std::uint32_t reuse = 4;
};
[[nodiscard]] constexpr SsrBudget ssr_budget(int quality) {
    switch (std::clamp(quality, 0, 2)) {
    case 0: return {32, 1};
    case 1: return {64, 4};
    default: return {128, 4};
    }
}

} // namespace engine::renderer
