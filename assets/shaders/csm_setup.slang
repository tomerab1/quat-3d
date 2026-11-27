// csm_setup.slang — GPU cascade fitting (ROADMAP R12).
//
// One workgroup of four threads, one per cascade: reads this frame's
// geometry depth bounds (hiz_build.slang), turns them into the shadowed
// view-distance range (quantised, with hysteresis against the range the
// previous frame kept — g_range_state, shared by every frame slot — and
// clamped to camera near .. max distance) and fits each cascade exactly as
// engine/renderer/csm.cpp does, writing the CascadeData the shadow and
// lighting passes read. No CPU read-back: the cascades follow the depth
// buffer within the same frame.

import csm;

// Mirrors CsmSetupParams in shadow_pass.cpp (std140, 176 bytes).
struct CsmSetupParams {
    float4 inv_view[4]; // view -> world (columns)
    float4 inv_proj[4]; // clip -> view (columns)
    float4 to_light;    // xyz: unit direction toward the sun
    float4 split;       // x count, y lambda, z max distance, w blend band
    float4 fit;         // x depth padding toward the sun (0 when pancaked), y map size,
                        // z use depth bounds (0/1), w caster reach beyond depth 0
}

ConstantBuffer<CsmSetupParams>   g_params;
StructuredBuffer<uint>           g_bounds; // depth min / max bits (hiz_build.slang)
RWStructuredBuffer<CascadeData>  g_cascades;
RWStructuredBuffer<float4>       g_range_state; // [0] = kept lo, hi, valid (csm.hpp CsmRangeState)

groupshared float gs_texel[CSM_MAX_CASCADES];
groupshared float gs_depth[CSM_MAX_CASCADES];
groupshared float gs_split[CSM_MAX_CASCADES];

[shader("compute")]
[numthreads(4, 1, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    uint count = clamp(uint(g_params.split.x), 1u, CSM_MAX_CASCADES);
    float lambda = g_params.split.y;
    float band = g_params.split.w;

    // Shadowed range (csm.cpp shadow_range).
    float cam_near = csm_view_distance(g_params.inv_proj, 0.0);
    float cam_far = csm_view_distance(g_params.inv_proj, 1.0);
    float full_far = min(g_params.split.z, cam_far);
    float lo = cam_near;
    float hi = full_far;
    bool fitted = false;
    float4 kept = float4(0.0, 0.0, 0.0, 0.0);
    if (g_params.fit.z > 0.5) {
        uint min_bits = g_bounds[0];
        uint max_bits = g_bounds[1];
        if (min_bits <= max_bits) {
            float2 q = csm_stabilized_range(
                csm_view_distance(g_params.inv_proj, asfloat(min_bits)),
                csm_view_distance(g_params.inv_proj, asfloat(max_bits)), g_range_state[0]);
            kept = float4(q, 1.0, 0.0);
            float tight_lo = max(cam_near, q.x);
            float tight_hi = min(full_far, q.y);
            if (tight_hi > tight_lo) {
                lo = tight_lo;
                hi = tight_hi;
                fitted = true;
            }
        }
    }

    uint c = min(i, count - 1);
    float start = csm_split(lo, hi, c, count, lambda);
    float end = csm_split(lo, hi, c + 1, count, lambda);
    if (c > 0) {
        start -= band * (start - csm_split(lo, hi, c - 1, count, lambda));
    }
    LightBasis light = csm_light_basis(g_params.to_light.xyz);
    CascadeFit f = csm_fit_slice(g_params.inv_view, g_params.inv_proj, light, start, end,
                                 g_params.fit.y, g_params.fit.x);
    for (uint k = 0; k < 4; ++k) {
        g_cascades[0].view_proj[i * 4 + k] = f.cols[k];
    }
    gs_texel[i] = f.texel;
    gs_depth[i] = f.depth_range;
    gs_split[i] = end;
    GroupMemoryBarrierWithGroupSync();
    if (i == 0) {
        g_cascades[0].split_far = float4(gs_split[0], gs_split[1], gs_split[2], gs_split[3]);
        g_cascades[0].texel_world = float4(gs_texel[0], gs_texel[1], gs_texel[2], gs_texel[3]);
        g_cascades[0].depth_range = float4(gs_depth[0], gs_depth[1], gs_depth[2], gs_depth[3]);
        g_cascades[0].params = float4(float(count), lo, band, fitted ? 1.0 : 0.0);
        g_cascades[0].limits = float4(full_far, g_params.fit.w, 0.0, 0.0);
        g_range_state[0] = kept; // every thread has read it before the barrier
    }
}
