#pragma once

// Tunable settings of the R12 shadow / ambient-occlusion stack: one POD per
// feature with the valid range of every numeric field next to it, so tools
// (the editor's Renderer panel, live tuning over MCP) can present and clamp
// them without knowing the renderer. sanitized() returns a copy with every
// field clamped into range; the passes call it on whatever they are given.

#include <algorithm>
#include <cstdint>

namespace engine::renderer {

struct FloatRange {
    float min = 0.0F;
    float max = 1.0F;
    [[nodiscard]] constexpr float clamp(float v) const { return std::clamp(v, min, max); }
};

struct IntRange {
    int min = 0;
    int max = 1;
    [[nodiscard]] constexpr int clamp(int v) const { return std::clamp(v, min, max); }
};

// Cascaded shadow maps for the sun (4 cascades, CLAUDE.md frame structure).
struct CsmSettings {
    bool          enabled = true;
    int           cascade_count = 4;   // cascades rendered
    float         split_lambda = 0.8F; // 0 = linear splits, 1 = logarithmic
    float         max_distance = 150.0F; // metres from the camera where shadows end
    // Fit the cascades to this frame's depth range (min/max geometry depth,
    // reduced on the GPU; SDSM-style). Off: the fixed range camera near ..
    // max_distance, computed on the CPU.
    bool          gpu_depth_bounds = true;
    float         blend_band = 0.1F;  // fraction of each cascade cross-faded into the next
    float         pcf_radius = 1.5F;  // filter radius in shadow-map texels
    // Sun angular diameter in degrees for the PCSS penumbra (blocker search +
    // variable filter radius); 0 keeps a fixed pcf_radius.
    float         light_size_deg = 0.53F;
    float         normal_bias = 1.0F; // receiver offset along the normal, in texels
    float         depth_bias = 1.0F;  // receiver offset toward the light, in texels
    // How far behind a cascade (toward the sun) casters are still captured.
    float         caster_distance = 200.0F;
    bool          debug_cascades = false; // tint each pixel by its cascade

    static constexpr IntRange  k_cascade_count{1, 4};
    static constexpr FloatRange k_split_lambda{0.0F, 1.0F};
    static constexpr FloatRange k_max_distance{5.0F, 5000.0F};
    static constexpr FloatRange k_blend_band{0.0F, 0.5F};
    static constexpr FloatRange k_pcf_radius{0.0F, 8.0F};
    static constexpr FloatRange k_light_size_deg{0.0F, 10.0F};
    static constexpr FloatRange k_normal_bias{0.0F, 8.0F};
    static constexpr FloatRange k_depth_bias{0.0F, 8.0F};
    static constexpr FloatRange k_caster_distance{1.0F, 10000.0F};

    [[nodiscard]] constexpr CsmSettings sanitized() const {
        CsmSettings s = *this;
        s.cascade_count = k_cascade_count.clamp(cascade_count);
        s.split_lambda = k_split_lambda.clamp(split_lambda);
        s.max_distance = k_max_distance.clamp(max_distance);
        s.blend_band = k_blend_band.clamp(blend_band);
        s.pcf_radius = k_pcf_radius.clamp(pcf_radius);
        s.light_size_deg = k_light_size_deg.clamp(light_size_deg);
        s.normal_bias = k_normal_bias.clamp(normal_bias);
        s.depth_bias = k_depth_bias.clamp(depth_bias);
        s.caster_distance = k_caster_distance.clamp(caster_distance);
        return s;
    }
};

// Ground-truth ambient occlusion (XeGTAO port) with bent normals.
struct GtaoSettings {
    bool          enabled = true;
    float         radius = 0.75F;        // world-space effect radius, metres
    float         falloff_range = 0.615F; // fraction of the radius fading out
    float         sample_distribution_power = 2.0F; // > 1 packs samples near the centre
    float         thin_occluder_compensation = 0.0F;
    float         final_value_power = 2.2F; // visibility exponent (strength)
    float         depth_mip_offset = 3.3F;  // Hi-Z mip bias for distant samples
    int           slice_count = 3;          // XeGTAO "High": 3 slices x 3 steps
    int           steps_per_slice = 3;
    int           denoise_passes = 1;       // edge-aware 3x3 passes (0 = raw)
    float         denoise_blur_beta = 1.2F;
    bool          bent_normals = true;      // IBL diffuse + specular occlusion
    bool          specular_occlusion = true;
    bool          multi_bounce = true;      // Jimenez 2016 albedo-aware bounce

    static constexpr FloatRange k_radius{0.05F, 10.0F};
    static constexpr FloatRange k_falloff_range{0.0F, 1.0F};
    static constexpr FloatRange k_sample_distribution_power{1.0F, 4.0F};
    static constexpr FloatRange k_thin_occluder_compensation{0.0F, 0.7F};
    static constexpr FloatRange k_final_value_power{0.5F, 5.0F};
    static constexpr FloatRange k_depth_mip_offset{0.0F, 8.0F};
    static constexpr IntRange  k_slice_count{1, 9};
    static constexpr IntRange  k_steps_per_slice{1, 8};
    static constexpr IntRange  k_denoise_passes{0, 3};
    static constexpr FloatRange k_denoise_blur_beta{0.1F, 10.0F};

    [[nodiscard]] constexpr GtaoSettings sanitized() const {
        GtaoSettings s = *this;
        s.radius = k_radius.clamp(radius);
        s.falloff_range = k_falloff_range.clamp(falloff_range);
        s.sample_distribution_power = k_sample_distribution_power.clamp(sample_distribution_power);
        s.thin_occluder_compensation =
            k_thin_occluder_compensation.clamp(thin_occluder_compensation);
        s.final_value_power = k_final_value_power.clamp(final_value_power);
        s.depth_mip_offset = k_depth_mip_offset.clamp(depth_mip_offset);
        s.slice_count = k_slice_count.clamp(slice_count);
        s.steps_per_slice = k_steps_per_slice.clamp(steps_per_slice);
        s.denoise_passes = k_denoise_passes.clamp(denoise_passes);
        s.denoise_blur_beta = k_denoise_blur_beta.clamp(denoise_blur_beta);
        return s;
    }
};

// Screen-space contact shadows: a short march toward the sun through the
// depth buffer, multiplied into the sun's shadow term.
struct ContactShadowSettings {
    bool          enabled = true;
    float         length = 0.35F;    // ray length toward the sun, metres
    int           steps = 12;        // depth samples along the ray
    float         thickness = 0.2F;  // how far behind the depth buffer a hit still counts, m
    float         intensity = 1.0F;  // 0 = no effect, 1 = full shadow on a hit

    static constexpr FloatRange k_length{0.01F, 4.0F};
    static constexpr IntRange  k_steps{2, 64};
    static constexpr FloatRange k_thickness{0.01F, 4.0F};
    static constexpr FloatRange k_intensity{0.0F, 1.0F};

    [[nodiscard]] constexpr ContactShadowSettings sanitized() const {
        ContactShadowSettings s = *this;
        s.length = k_length.clamp(length);
        s.steps = k_steps.clamp(steps);
        s.thickness = k_thickness.clamp(thickness);
        s.intensity = k_intensity.clamp(intensity);
        return s;
    }
};

} // namespace engine::renderer
