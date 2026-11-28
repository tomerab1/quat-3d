// grass_ground.slang — ground cover under and beyond the grass (grass v2).
//
// One thread per GBuffer pixel, after the opaque ground writers (mesh,
// terrain) and before the blades. A pixel whose reconstructed world position
// lies on the ground the grass grows on (same height source, slope and
// height limits as grass_generate) gets its albedo tinted:
//   * near the camera toward a thatch colour, stronger under the clumps, so
//     the gaps between tufts read as low undergrowth rather than bare soil;
//   * toward the blades' mean colour from the thinning start to the radius
//     and beyond, so the field keeps its colour where blades thin out and
//     does not end in a visible ring at the radius. On flat ground this
//     reaches the horizon; over terrain it fades out between the radius and
//     the edge of the bound height tiles (at most 1.5 x radius), the only
//     ground whose height the pass knows.
// This is the terrain-colour half of the usual grass/ground blend (Ghost of
// Tsushima: Wohllaib, "Procedural Grass in 'Ghost of Tsushima'", GDC 2021 —
// grass colour sampled from, and fading into, the terrain albedo; the
// blades' root tint in grass_generate is the other half). The tint keeps the
// ground texture's detail: its luminance relative to the cover colour
// modulates the result.

import grass_common;

[[vk::binding(0)]] ConstantBuffer<GrassParams> g_params;
[[vk::binding(1)]] SamplerState                g_sampler; // linear, clamp to edge
[[vk::binding(2)]] Texture2D                   g_height0;
[[vk::binding(3)]] Texture2D                   g_height1;
[[vk::binding(4)]] Texture2D                   g_height2;
[[vk::binding(5)]] Texture2D                   g_height3;
[[vk::binding(6)]] Texture2D                   g_depth;  // GBuffer depth
[[vk::binding(7)]] [format("rgba8")] RWTexture2D<float4> g_albedo; // GBuffer albedo, in place

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id : SV_DispatchThreadID) {
    if (any(float2(id.xy) >= g_params.screen.xy)) return;
    const float depth = g_depth.Load(int3(id.xy, 0)).r;
    if (depth >= 1.0) return; // sky
    const float2 ndc = (float2(id.xy) + 0.5) * g_params.screen.zw * 2.0 - 1.0;
    const float4 h = mul(g_params.inv_view_proj, float4(ndc, depth, 1.0));
    const float3 world = h.xyz / h.w;

    float ground_y;
    float3 n;
    if (!grass_ground_at(g_params, g_height0, g_height1, g_height2, g_height3, g_sampler,
                         world.xz, ground_y, n)) {
        return;
    }
    if (!grass_ground_allowed(g_params, ground_y, n)) return;
    const float dist = length(world - g_params.camera.xyz);
    // On the ground (not a rock, log or stem above it): a few centimetres,
    // growing with distance for depth precision and terrain tessellation.
    if (abs(world.y - ground_y) > 0.03 + 0.004 * dist) return;

    // 0 near the camera, 1 from the thinning start to the radius and beyond.
    const float far = smoothstep(g_params.cell.w, g_params.cell.y, dist);
    float cover = lerp(g_params.footprint.z, g_params.footprint.w, far) *
                  grass_slope_keep(g_params, n.y);
    // Near the camera the cover follows the clumps: densest at their centres.
    if (g_params.clump.x > 0.0) {
        const Clump clump = grass_clump(g_params, world.xz);
        const float r = length(clump.centre - world.xz) / g_params.clump.x;
        cover *= lerp(lerp(1.25, 0.55, saturate(r * 1.4)), 1.0, far);
    }
    // Fade out before the edge of the bound height tiles: beyond them there
    // is no ground height to match (grass_cover_fade).
    if (g_params.clump.w > 0.0) {
        const float dist_xz = length(world.xz - g_params.camera.xz);
        cover *= 1.0 - smoothstep(g_params.ground.w, g_params.clump.w, dist_xz);
    }
    cover = saturate(cover);
    if (cover <= 0.0) return;

    const float3 tint = lerp(g_params.cover_color.rgb, g_params.far_color.rgb, far);
    const float4 albedo = g_albedo[id.xy];
    const float detail =
        sqrt(clamp(luminance(albedo.rgb) / max(luminance(tint), 1e-3), 0.5, 2.0));
    g_albedo[id.xy] = float4(lerp(albedo.rgb, saturate(tint * detail), cover), albedo.a);
}
