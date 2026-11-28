#pragma once

// Layered terrain material (ROADMAP R26.1, issue #15).
//
// A terrain is shaded by a stack of up to `max_material_layers` layers, each
// a PBR texture set (albedo, normal, packed roughness / AO / height) placed by
// procedural rules — slope, altitude, curvature and flow (the erosion
// gullies), a noise break-up — and optionally by painted weights (terrain edit
// paint channels, G19). The stack composites bottom to top like a splat
// stack: layer 0 is the ground everything else lies on (full coverage), each
// later layer covers what lies below it by its rule coverage. Neighbouring
// layers then trade places by their texture heights (height blending:
// Mittring 2007; Mishkinis 2013), so rock pokes through a thin grass edge
// instead of cross-fading into it.
//
// This header is the data model and the CPU reference of the blend maths;
// assets/shaders/lib/terrain_material.slang evaluates the same rules on the
// GPU (the reference is what the tests pin, and the GPU test checks the
// shader against it). Presets give a good-looking ground without
// authoring: temperate_meadow (the default), alpine, desert, forest_floor.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace engine::terrain {

inline constexpr std::size_t max_material_layers = 8;

// Built-in texture sets a layer can use (assets/terrain/layers.json lists the
// CC0 Poly Haven source of each). A set that is not fetched / cooked falls
// back to the layer's tint with procedural detail.
inline constexpr std::array<std::string_view, 7> texture_set_names = {
    "grass", "dirt", "rock", "cliff", "forest_floor", "sand", "snow"};

// Which edit paint channel (G19 terrain edits: RGBA8 per tile, r dirt,
// g gravel, b garden soil) adds painted weight to a layer.
enum class PaintChannel : std::uint8_t { none, r, g, b };

// Altitude ranges are world heights, or heights relative to the terrain's
// snowline (so a snow layer follows the `snowline` field).
enum class AltitudeRef : std::uint8_t { world, snowline };

// Where a layer lies. Coverage = slope band x altitude band x curvature and
// flow affinities x strength, with the slope / altitude inputs jittered by a
// noise so transitions are not contour lines.
struct LayerRule {
    glm::vec2   slope_deg{0.0F, 90.0F};          // band, degrees from horizontal
    float       slope_softness_deg = 4.0F;       // half-width of each band edge
    glm::vec2   altitude_m{-100000.0F, 100000.0F};
    float       altitude_softness_m = 8.0F;
    AltitudeRef altitude_ref = AltitudeRef::world;
    // -1..1: > 0 prefers concave ground (gullies, hollows), < 0 convex
    // (ridges, crests); 0 ignores curvature.
    float concavity = 0.0F;
    // -1..1: > 0 prefers ground where water collects (the flow accumulation
    // of the heightmap), < 0 dry ground; 0 ignores flow.
    float flow = 0.0F;
    float noise = 0.35F;    // 0..1: jitter of the band edges (noise break-up)
    float strength = 1.0F;  // 0..1: overall coverage multiplier
    PaintChannel paint = PaintChannel::none;
};

struct MaterialLayer {
    std::string name;          // label (editor / tools)
    std::string texture_set;   // one of texture_set_names
    glm::vec3   tint{1.0F};    // linear multiplier on the albedo (or the colour, untextured)
    float       uv_scale_m = 4.0F;        // metres one texture repeat covers
    float       normal_strength = 1.0F;   // 0..2
    float       roughness = 1.0F;         // 0..2: multiplies the texture's roughness
    float       height_offset = 0.0F;     // -1..1: bias of the texture height in blending
    LayerRule   rule;
};

struct TerrainMaterial {
    std::string                preset;   // preset it started from ("" = custom)
    std::vector<MaterialLayer> layers;   // bottom (layer 0, full coverage) to top
    // Height-blend transition depth in texture-height units (0.01..1):
    // small = crisp, height-driven transitions; large = soft cross-fades.
    float blend_depth = 0.15F;
    // Slopes steeper than this (degrees) switch from a top-down projection
    // to triplanar mapping (no stretching on cliffs), over +-5 degrees.
    float triplanar_slope_deg = 38.0F;
    float triplanar_sharpness = 4.0F;     // 1..16: plane blend exponent
    // Tiling break-up: hex-tiled stochastic texturing (Heitz & Neyret 2018;
    // Mikkelsen 2022) of the top-down projection.
    bool  stochastic_tiling = true;
    // Large-scale albedo variation (0..1) and its feature size in metres.
    float macro_strength = 0.25F;
    float macro_scale_m = 120.0F;
    // Beyond far_start_m the layers fade over far_range_m to their mean
    // colour (the distance-dependent macro colour: no tiling, no moire).
    float far_start_m = 90.0F;
    float far_range_m = 160.0F;
};

// Preset names, in the order tools list them.
inline constexpr std::array<std::string_view, 4> material_preset_names = {
    "temperate_meadow", "alpine", "desert", "forest_floor"};

// The named preset; nullopt for an unknown name. Display names with spaces
// ("temperate meadow") are accepted too.
[[nodiscard]] std::optional<TerrainMaterial> material_preset(std::string_view name);

// The default terrain material (temperate_meadow).
[[nodiscard]] TerrainMaterial default_terrain_material();

[[nodiscard]] bool is_texture_set(std::string_view name);

// ---- CPU reference of the blend maths (mirrors lib/terrain_material.slang) --

// What the rules read at one ground point.
struct SurfacePoint {
    float     slope_deg = 0.0F;
    float     height_m = 0.0F;     // world height
    float     snowline_m = 0.0F;   // world height of the snowline
    float     concavity = 0.0F;    // -1 (ridge) .. 1 (gully)
    float     flow = 0.0F;         // 0 (dry) .. 1 (water collects)
    float     noise = 0.5F;        // 0..1 break-up noise at the point
    glm::vec4 paint{0.0F};         // edit paint channels (r, g, b used)
};

using LayerWeights = std::array<float, max_material_layers>;

// Coverage of one layer's rule at `p`, in [0, 1] (before compositing).
[[nodiscard]] float rule_coverage(const LayerRule& rule, const SurfacePoint& p);

// Final layer weights at `p`: rule coverages composited bottom to top (layer
// 0 covers everything), then painted weights laid over them. Sums to 1 when
// the material has layers; unused entries are 0.
[[nodiscard]] LayerWeights layer_weights(const TerrainMaterial& material, const SurfacePoint& p);

// Height blending of `weights` with per-layer texture heights (0..1, offset
// already applied): layers within `depth` of the highest (height + weight)
// share the point, by how far they reach into that band. Layers with zero
// weight never appear. Sums to 1 (unless every weight is 0).
[[nodiscard]] LayerWeights height_blend(const LayerWeights& weights,
                                        std::span<const float> heights, float depth);

} // namespace engine::terrain
