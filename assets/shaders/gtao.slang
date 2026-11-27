// gtao.slang — ground-truth ambient occlusion with bent normals (ROADMAP R12).
//
// A Slang port of the main pass of Intel's XeGTAO (MIT licence,
// https://github.com/GameTechDev/XeGTAO, Filip Strugar / Steve Mccalla),
// itself an implementation of Jimenez, Wu, Pesce, Jarabo, "Practical Realtime
// Strategies for Accurate Indirect Occlusion" (SIGGRAPH 2016), Algorithms 1
// and 2 (bent normals). Per pixel: `slices` directions around the view
// vector; along each, march the depth both ways for the two horizon angles,
// then integrate the cosine-weighted visible arc against the projected
// normal. Samples read the Hi-Z pyramid's nearest-depth channel at a mip that
// grows with the sample distance (XeGTAO's depth MIP chain). Noise is
// XeGTAO's spatio-temporal Hilbert-curve R2 sequence, advanced per frame so
// TAA accumulates it; gtao_denoise.slang is the spatial filter.
//
// XeGTAO works in a left-handed view space (x right, y up, z forward); the
// engine's is right-handed (z backward), so z flips on the way in and out.
// Output: rgb = world-space bent normal * 0.5 + 0.5, a = visibility; plus
// packed depth edges for the denoiser.

import octahedral;

static const float XE_PI = 3.1415926535897932384626433832795;
static const float XE_PI_HALF = 1.5707963267948966192313216916398;
static const uint  XE_HILBERT_LEVEL = 6u;
static const uint  XE_HILBERT_WIDTH = 1u << XE_HILBERT_LEVEL;

// Mirrors GtaoConstants in gtao_pass.cpp (std140, 224 bytes).
struct GtaoConstants {
    float4 view[4];      // world -> view (columns)
    float4 inv_view[4];  // view -> world (columns)
    float4 ndc_to_view;  // xy = NDCToViewMul, zw = NDCToViewAdd (screen uv -> view xy / depth)
    float4 pixel;        // xy = 1 / extent, zw = NDCToViewMul * pixel size
    float4 params0;      // effect radius, falloff range, sample distribution power, thin occluder
    float4 params1;      // final value power, depth mip offset, slices, steps per slice
    float4 params2;      // depth -> view (A, B), noise index, max mip
    float4 params3;      // x bent normals
}
ConstantBuffer<GtaoConstants> g_consts;

Texture2D                            g_hiz;       // r = nearest depth (Hi-Z pyramid)
Texture2D                            g_normal;    // GBuffer octahedral world normal
SamplerState                         g_point;     // nearest, clamp
[format("rgba8")] RWTexture2D<float4> g_ao_out;   // bent normal + visibility
[format("r8")] RWTexture2D<float>     g_edges_out; // packed LRTB edges

float3 mul_cols3(float4 cols[4], float3 v) {
    return cols[0].xyz * v.x + cols[1].xyz * v.y + cols[2].xyz * v.z;
}

// NDC depth -> positive view-space depth (perspective).
float linear_depth(float d) {
    return g_consts.params2.y / (d + g_consts.params2.x);
}

float3 view_position(float2 screen_pos, float view_z) {
    return float3((g_consts.ndc_to_view.xy * screen_pos + g_consts.ndc_to_view.zw) * view_z,
                  view_z);
}

float fast_acos(float in_x) {
    float x = abs(in_x);
    float res = -0.156583 * x + XE_PI_HALF;
    res *= sqrt(1.0 - x);
    return in_x >= 0.0 ? res : XE_PI - res;
}

float4 calculate_edges(float center_z, float left_z, float right_z, float top_z,
                       float bottom_z) {
    float4 edges = float4(left_z, right_z, top_z, bottom_z) - center_z;
    float slope_lr = (edges.y - edges.x) * 0.5;
    float slope_tb = (edges.w - edges.z) * 0.5;
    float4 adjusted = edges + float4(slope_lr, -slope_lr, slope_tb, -slope_tb);
    edges = min(abs(edges), abs(adjusted));
    return saturate(1.25 - edges / (center_z * 0.011));
}

float pack_edges(float4 edges) {
    edges = round(saturate(edges) * 2.9);
    return dot(edges, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

uint hilbert_index(uint pos_x, uint pos_y) {
    uint index = 0u;
    for (uint level = XE_HILBERT_WIDTH / 2u; level > 0u; level /= 2u) {
        uint region_x = (pos_x & level) > 0u ? 1u : 0u;
        uint region_y = (pos_y & level) > 0u ? 1u : 0u;
        index += level * level * ((3u * region_x) ^ region_y);
        if (region_y == 0u) {
            if (region_x == 1u) {
                pos_x = (XE_HILBERT_WIDTH - 1u) - pos_x;
                pos_y = (XE_HILBERT_WIDTH - 1u) - pos_y;
            }
            uint t = pos_x;
            pos_x = pos_y;
            pos_y = t;
        }
    }
    return index;
}

float2 spatio_temporal_noise(uint2 pix, uint temporal_index) {
    uint index = hilbert_index(pix.x % XE_HILBERT_WIDTH, pix.y % XE_HILBERT_WIDTH);
    index += 288u * (temporal_index % 64u);
    return frac(0.5 + float(index) * float2(0.75487766624669276005, 0.5698402909980532659114));
}

float3x3 rot_from_to(float3 from, float3 to) {
    float e = dot(from, to);
    if (abs(e) > 1.0 - 0.0003) {
        return float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    }
    float3 v = cross(from, to);
    float h = 1.0 / (1.0 + e);
    float hvx = h * v.x;
    float hvz = h * v.z;
    float hvxy = hvx * v.y;
    float hvxz = hvx * v.z;
    float hvyz = hvz * v.y;
    return float3x3(e + hvx * v.x, hvxy - v.z, hvxz + v.y,
                    hvxy + v.z, e + h * v.y * v.y, hvyz - v.x,
                    hvxz - v.y, hvyz + v.x, e + hvz * v.z);
}

float load_view_z(int2 p, int2 size) {
    return linear_depth(g_hiz.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).r);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0;
    uint h = 0;
    g_ao_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) {
        return;
    }
    int2 pix = int2(tid.xy);
    int2 size = int2(int(w), int(h));
    float raw_depth = g_hiz.Load(int3(pix, 0)).r;
    if (raw_depth >= 1.0) { // background: open, and an edge on every side
        g_ao_out[pix] = float4(0.5, 1.0, 0.5, 1.0);
        g_edges_out[pix] = 0.0;
        return;
    }

    float view_z = linear_depth(raw_depth);
    float left_z = load_view_z(pix + int2(-1, 0), size);
    float right_z = load_view_z(pix + int2(1, 0), size);
    float top_z = load_view_z(pix + int2(0, -1), size);
    float bottom_z = load_view_z(pix + int2(0, 1), size);
    g_edges_out[pix] = pack_edges(calculate_edges(view_z, left_z, right_z, top_z, bottom_z));

    // GBuffer world normal -> XeGTAO view space.
    float3 n_world = oct_decode(g_normal.Load(int3(pix, 0)).xy);
    float3 n_view = normalize(mul_cols3(g_consts.view, n_world));
    float3 normal = float3(n_view.x, n_view.y, -n_view.z);

    // Pull the centre towards the camera against depth imprecision (fp32).
    view_z *= 0.99999;
    float2 screen_pos = (float2(pix) + 0.5) * g_consts.pixel.xy;
    float3 centre = view_position(screen_pos, view_z);
    float3 view_vec = normalize(-centre);

    float effect_radius = g_consts.params0.x;
    float distribution_power = g_consts.params0.z;
    float thin_compensation = g_consts.params0.w;
    float falloff_range = g_consts.params0.y * effect_radius;
    float falloff_from = effect_radius * (1.0 - g_consts.params0.y);
    float falloff_mul = -1.0 / falloff_range;
    float falloff_add = falloff_from / falloff_range + 1.0;
    float slice_count = g_consts.params1.z;
    float steps_per_slice = g_consts.params1.w;
    float mip_offset = g_consts.params1.y;
    float max_mip = g_consts.params2.w;

    float2 noise = spatio_temporal_noise(uint2(pix), uint(g_consts.params2.z));
    float noise_slice = noise.x;
    float noise_sample = noise.y;

    float visibility = 0.0;
    float3 bent_normal = float3(0.0, 0.0, 0.0);

    const float pixel_too_close = 1.3;
    float2 pixel_size_at_z = view_z.xx * g_consts.pixel.zw;
    float screen_radius = effect_radius / pixel_size_at_z.x;
    // Fade out for small screen radii.
    visibility += saturate((10.0 - screen_radius) / 100.0) * 0.5;
    float min_s = pixel_too_close / screen_radius;

    for (float slice = 0.0; slice < slice_count; slice += 1.0) {
        float slice_k = (slice + noise_slice) / slice_count;
        float phi = slice_k * XE_PI;
        float cos_phi = cos(phi);
        float sin_phi = sin(phi);
        float2 omega = float2(cos_phi, -sin_phi) * screen_radius;

        float3 direction = float3(cos_phi, sin_phi, 0.0);
        float3 ortho_direction = direction - dot(direction, view_vec) * view_vec;
        float3 axis = normalize(cross(ortho_direction, view_vec));
        float3 projected_normal = normal - axis * dot(normal, axis);
        float sign_norm = float(sign(dot(ortho_direction, projected_normal)));
        float projected_length = length(projected_normal);
        float cos_norm = saturate(dot(projected_normal, view_vec) / projected_length);
        float n = sign_norm * fast_acos(cos_norm);

        float low_horizon_cos0 = cos(n + XE_PI_HALF);
        float low_horizon_cos1 = cos(n - XE_PI_HALF);
        float horizon_cos0 = low_horizon_cos0;
        float horizon_cos1 = low_horizon_cos1;

        for (float step = 0.0; step < steps_per_slice; step += 1.0) {
            float step_base_noise = (slice + step * steps_per_slice) * 0.6180339887498948482;
            float step_noise = frac(noise_sample + step_base_noise);
            float s = (step + step_noise) / steps_per_slice;
            s = pow(s, distribution_power);
            s += min_s;
            float2 sample_offset = s * omega;
            float sample_offset_length = length(sample_offset);
            float mip = clamp(log2(sample_offset_length) - mip_offset, 0.0, max_mip);
            sample_offset = round(sample_offset) * g_consts.pixel.xy;

            float2 sample_pos0 = screen_pos + sample_offset;
            float z0 = linear_depth(g_hiz.SampleLevel(g_point, sample_pos0, mip).r);
            float3 delta0 = view_position(sample_pos0, z0) - centre;
            float2 sample_pos1 = screen_pos - sample_offset;
            float z1 = linear_depth(g_hiz.SampleLevel(g_point, sample_pos1, mip).r);
            float3 delta1 = view_position(sample_pos1, z1) - centre;

            float dist0 = length(delta0);
            float dist1 = length(delta1);
            float3 horizon_vec0 = delta0 / dist0;
            float3 horizon_vec1 = delta1 / dist1;

            // Thin-occluder heuristic: samples behind the centre fall off sooner.
            float falloff_base0 =
                length(float3(delta0.x, delta0.y, delta0.z * (1.0 + thin_compensation)));
            float falloff_base1 =
                length(float3(delta1.x, delta1.y, delta1.z * (1.0 + thin_compensation)));
            float weight0 = saturate(falloff_base0 * falloff_mul + falloff_add);
            float weight1 = saturate(falloff_base1 * falloff_mul + falloff_add);

            float shc0 = dot(horizon_vec0, view_vec);
            float shc1 = dot(horizon_vec1, view_vec);
            shc0 = lerp(low_horizon_cos0, shc0, weight0);
            shc1 = lerp(low_horizon_cos1, shc1, weight1);
            horizon_cos0 = max(horizon_cos0, shc0);
            horizon_cos1 = max(horizon_cos1, shc1);
        }

        // XeGTAO's fudge against slight over-darkening on high slopes.
        projected_length = lerp(projected_length, 1.0, 0.05);

        float h0 = -fast_acos(horizon_cos1);
        float h1 = fast_acos(horizon_cos0);
        float iarc0 = (cos_norm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
        float iarc1 = (cos_norm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
        visibility += projected_length * (iarc0 + iarc1);

        // Algorithm 2: the bent normal of this slice.
        float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) -
                    sin(3.0 * h1 - n) + 16.0 * sin(n) - 3.0 * (sin(h0 + n) + sin(h1 + n))) /
                   12.0;
        float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) -
                    3.0 * (cos(h0 + n) + cos(h1 + n))) /
                   12.0;
        float3 local_bent = float3(direction.x * t0, direction.y * t0, -t1);
        local_bent = mul(rot_from_to(float3(0.0, 0.0, -1.0), view_vec), local_bent) *
                     projected_length;
        bent_normal += local_bent;
    }
    visibility /= slice_count;
    visibility = pow(max(visibility, 0.0), g_consts.params1.x);
    visibility = max(0.03, visibility); // never fully black: the pixel is visible

    float3 bent_view = g_consts.params3.x > 0.5 ? normalize(bent_normal) : normal;
    // XeGTAO view space -> engine view space -> world.
    float3 bent_world =
        normalize(mul_cols3(g_consts.inv_view, float3(bent_view.x, bent_view.y, -bent_view.z)));
    g_ao_out[pix] = float4(bent_world * 0.5 + 0.5, saturate(visibility));
}
