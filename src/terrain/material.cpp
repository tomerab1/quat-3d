// Layered terrain material: presets and the CPU reference of the blend maths
// (see engine/terrain/material.hpp; mirrored by lib/terrain_material.slang).

#include "engine/terrain/material.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace engine::terrain {

namespace {

// Band edges at or past these are open (no fade on that side).
constexpr float k_open_low_altitude = -99999.0F;
constexpr float k_open_high_altitude = 99999.0F;

[[nodiscard]] float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// 1 inside [lo, hi], fading over +-soft at each edge; open edges do not fade.
[[nodiscard]] float band(float x, float lo, float hi, float soft, bool open_lo, bool open_hi) {
    const float s = std::max(soft, 1e-3F);
    const float rise = open_lo ? 1.0F : smoothstep(lo - s, lo + s, x);
    const float fall = open_hi ? 1.0F : 1.0F - smoothstep(hi - s, hi + s, x);
    return rise * fall;
}

// Affinity -1..1 toward a condition of strength `s` (0..1): 1 ignores it,
// +1 keeps the layer only where the condition holds, -1 only where it does not.
[[nodiscard]] float affinity(float k, float s) {
    k = std::clamp(k, -1.0F, 1.0F);
    const float target = k >= 0.0F ? s : 1.0F - s;
    return 1.0F + (target - 1.0F) * std::abs(k);
}

[[nodiscard]] std::string canonical(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        out.push_back(c == ' ' || c == '-' ? '_'
                                           : static_cast<char>(std::tolower(
                                                 static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] MaterialLayer layer(std::string name, std::string set, glm::vec3 tint, float uv_m,
                                  LayerRule rule = {}) {
    MaterialLayer l;
    l.name = std::move(name);
    l.texture_set = std::move(set);
    l.tint = tint;
    l.uv_scale_m = uv_m;
    l.rule = rule;
    return l;
}

[[nodiscard]] LayerRule slope_rule(float lo, float hi, float soft, float noise) {
    LayerRule r;
    r.slope_deg = {lo, hi};
    r.slope_softness_deg = soft;
    r.noise = noise;
    return r;
}

// Gullies and hollows where water runs: bare, wet soil.
[[nodiscard]] LayerRule gully_rule(float max_slope, float strength) {
    LayerRule r = slope_rule(0.0F, max_slope, 5.0F, 0.5F);
    r.flow = 1.0F;
    r.concavity = 0.35F;
    r.strength = strength;
    return r;
}

[[nodiscard]] LayerRule snow_rule(float max_slope) {
    LayerRule r = slope_rule(0.0F, max_slope, 6.0F, 0.6F);
    r.altitude_m = {0.0F, 100000.0F};
    r.altitude_softness_m = 12.0F;
    r.altitude_ref = AltitudeRef::snowline;
    return r;
}

[[nodiscard]] TerrainMaterial temperate_meadow() {
    TerrainMaterial m;
    m.preset = "temperate_meadow";
    LayerRule dirt = gully_rule(32.0F, 0.9F);
    dirt.paint = PaintChannel::r;
    LayerRule rock = slope_rule(33.0F, 90.0F, 5.0F, 0.5F);
    rock.paint = PaintChannel::g;
    m.layers = {
        layer("meadow", "grass", {1.0F, 1.0F, 1.0F}, 3.0F),
        layer("dirt", "dirt", {1.0F, 1.0F, 1.0F}, 3.5F, dirt),
        layer("rock", "rock", {1.0F, 1.0F, 1.0F}, 6.0F, rock),
        layer("snow", "snow", {1.0F, 1.0F, 1.0F}, 5.0F, snow_rule(52.0F)),
    };
    m.layers[2].height_offset = 0.1F; // rock pokes through the grass edge
    return m;
}

[[nodiscard]] TerrainMaterial alpine() {
    TerrainMaterial m;
    m.preset = "alpine";
    m.blend_depth = 0.12F;
    m.triplanar_slope_deg = 34.0F;
    LayerRule dirt = gully_rule(30.0F, 0.85F);
    dirt.paint = PaintChannel::r;
    LayerRule scree = slope_rule(26.0F, 90.0F, 5.0F, 0.6F);
    scree.paint = PaintChannel::g;
    m.layers = {
        layer("alpine grass", "grass", {0.85F, 0.9F, 0.8F}, 3.0F),
        layer("dirt", "dirt", {1.0F, 1.0F, 1.0F}, 3.5F, dirt),
        layer("scree", "rock", {1.0F, 1.0F, 1.0F}, 5.0F, scree),
        layer("cliff", "cliff", {1.0F, 1.0F, 1.0F}, 14.0F, slope_rule(44.0F, 90.0F, 5.0F, 0.4F)),
        layer("snow", "snow", {1.0F, 1.0F, 1.0F}, 5.0F, snow_rule(58.0F)),
    };
    m.layers[3].height_offset = 0.15F;
    return m;
}

[[nodiscard]] TerrainMaterial desert() {
    TerrainMaterial m;
    m.preset = "desert";
    m.blend_depth = 0.2F;
    m.macro_strength = 0.3F;
    LayerRule hardpan = gully_rule(25.0F, 0.75F);
    hardpan.paint = PaintChannel::r;
    LayerRule rock = slope_rule(24.0F, 90.0F, 5.0F, 0.5F);
    rock.paint = PaintChannel::g;
    m.layers = {
        layer("sand", "sand", {1.0F, 1.0F, 1.0F}, 4.0F),
        layer("hardpan", "dirt", {1.05F, 0.95F, 0.85F}, 4.0F, hardpan),
        layer("rock", "rock", {1.1F, 0.95F, 0.82F}, 6.0F, rock),
        layer("cliff", "cliff", {1.1F, 0.93F, 0.8F}, 14.0F, slope_rule(42.0F, 90.0F, 5.0F, 0.4F)),
    };
    return m;
}

[[nodiscard]] TerrainMaterial forest_floor() {
    TerrainMaterial m;
    m.preset = "forest_floor";
    m.macro_strength = 0.3F;
    LayerRule clearing = slope_rule(0.0F, 24.0F, 5.0F, 1.0F);
    clearing.concavity = -0.5F; // grass on the drier crests
    clearing.flow = -0.4F;
    clearing.strength = 0.65F;
    LayerRule dirt = gully_rule(32.0F, 0.85F);
    dirt.paint = PaintChannel::r;
    LayerRule rock = slope_rule(36.0F, 90.0F, 5.0F, 0.5F);
    rock.paint = PaintChannel::g;
    LayerRule soil = slope_rule(0.0F, 90.0F, 5.0F, 0.0F);
    soil.strength = 0.0F; // painted only (garden soil)
    soil.paint = PaintChannel::b;
    m.layers = {
        layer("forest floor", "forest_floor", {1.0F, 1.0F, 1.0F}, 3.0F),
        layer("clearing", "grass", {0.9F, 0.95F, 0.85F}, 3.0F, clearing),
        layer("dirt", "dirt", {0.85F, 0.85F, 0.85F}, 3.5F, dirt),
        layer("rock", "rock", {1.0F, 1.0F, 1.0F}, 6.0F, rock),
        layer("soil", "dirt", {0.6F, 0.55F, 0.5F}, 3.0F, soil),
    };
    return m;
}

} // namespace

std::optional<TerrainMaterial> material_preset(std::string_view name) {
    const std::string key = canonical(name);
    if (key == "temperate_meadow" || key == "meadow") return temperate_meadow();
    if (key == "alpine") return alpine();
    if (key == "desert") return desert();
    if (key == "forest_floor" || key == "forest") return forest_floor();
    return std::nullopt;
}

TerrainMaterial default_terrain_material() { return temperate_meadow(); }

bool is_texture_set(std::string_view name) {
    return std::ranges::find(texture_set_names, name) != texture_set_names.end();
}

float rule_coverage(const LayerRule& rule, const SurfacePoint& p) {
    const float jitter = (p.noise * 2.0F - 1.0F) * std::clamp(rule.noise, 0.0F, 1.0F);
    const float slope = p.slope_deg + jitter * 2.0F * rule.slope_softness_deg;
    const float height_ref = rule.altitude_ref == AltitudeRef::snowline ? p.snowline_m : 0.0F;
    const float altitude =
        p.height_m - height_ref + jitter * 2.0F * rule.altitude_softness_m;

    float c = band(slope, rule.slope_deg.x, rule.slope_deg.y, rule.slope_softness_deg,
                   rule.slope_deg.x <= 0.0F, rule.slope_deg.y >= 90.0F);
    c *= band(altitude, rule.altitude_m.x, rule.altitude_m.y, rule.altitude_softness_m,
              rule.altitude_m.x <= k_open_low_altitude, rule.altitude_m.y >= k_open_high_altitude);
    // Concavity: strength of the hollow (k > 0) or of the crest (k < 0).
    const float shape =
        smoothstep(0.0F, 0.5F, rule.concavity >= 0.0F ? p.concavity : -p.concavity);
    c *= affinity(std::abs(rule.concavity), shape);
    c *= affinity(rule.flow, smoothstep(0.35F, 0.75F, p.flow));
    return std::clamp(c * std::clamp(rule.strength, 0.0F, 1.0F), 0.0F, 1.0F);
}

LayerWeights layer_weights(const TerrainMaterial& material, const SurfacePoint& p) {
    LayerWeights w{};
    const std::size_t n = std::min(material.layers.size(), max_material_layers);
    if (n == 0) return w;
    // Composite top down: each layer takes its coverage of what is left.
    float remaining = 1.0F;
    for (std::size_t i = n; i-- > 1;) {
        const float c = rule_coverage(material.layers[i].rule, p);
        w[i] = c * remaining;
        remaining -= w[i];
    }
    w[0] = remaining;

    // Painted weights override the procedural stack.
    LayerWeights painted{};
    float paint_sum = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        switch (material.layers[i].rule.paint) {
            case PaintChannel::r: painted[i] = p.paint.r; break;
            case PaintChannel::g: painted[i] = p.paint.g; break;
            case PaintChannel::b: painted[i] = p.paint.b; break;
            case PaintChannel::none: break;
        }
        painted[i] = std::clamp(painted[i], 0.0F, 1.0F);
        paint_sum += painted[i];
    }
    if (paint_sum > 0.0F) {
        const float cover = std::min(paint_sum, 1.0F);
        const float norm = 1.0F / std::max(paint_sum, 1.0F);
        for (std::size_t i = 0; i < n; ++i) w[i] = w[i] * (1.0F - cover) + painted[i] * norm;
    }
    return w;
}

LayerWeights height_blend(const LayerWeights& weights, std::span<const float> heights,
                          float depth) {
    const float d = std::max(depth, 1e-3F);
    float top = -1e30F;
    for (std::size_t i = 0; i < weights.size() && i < heights.size(); ++i) {
        if (weights[i] > 0.0F) top = std::max(top, heights[i] + weights[i]);
    }
    LayerWeights out{};
    float sum = 0.0F;
    for (std::size_t i = 0; i < weights.size() && i < heights.size(); ++i) {
        if (weights[i] <= 0.0F) continue;
        out[i] = std::max(heights[i] + weights[i] - (top - d), 0.0F);
        sum += out[i];
    }
    if (sum > 0.0F) {
        for (float& v : out) v /= sum;
    }
    return out;
}

} // namespace engine::terrain
