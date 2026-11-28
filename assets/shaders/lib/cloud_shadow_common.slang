// cloud_shadow_common.slang — reading the cloud shadow map (R13.1).
//
// CloudShadowPass (cloud_shadow.slang) integrates the cloud layer's optical
// depth along the sun direction into a 2D texture: an orthographic
// projection along the sun, centred on the camera and snapped to whole
// texels. Each texel holds the sun's transmittance through the clouds along
// one line parallel to the sun, so every point on that line below the cloud
// base shares it — ground, grass, transparents and (R13.3) fog samples at any
// height read the same map with one bilinear fetch.
//
// Mirrors engine::renderer::CloudShadowProjection: uv = (dot(P, u.xyz) + u.w,
// dot(P, v.xyz) + v.w) for a world position P. Outside [0, 1] the clamped
// edge texel is read.

public struct CloudShadowProjection {
    public float4 u_axis; // xyz light-space right / extent, w offset
    public float4 v_axis; // xyz light-space up / extent, w offset
}

public float2 cloud_shadow_uv(CloudShadowProjection p, float3 world_pos) {
    return float2(dot(world_pos, p.u_axis.xyz) + p.u_axis.w,
                  dot(world_pos, p.v_axis.xyz) + p.v_axis.w);
}

// Sun transmittance through the clouds at `world_pos` (1 = clear). `samp`
// must be bilinear with clamp-to-edge addressing.
public float cloud_shadow_sample(Texture2D map, SamplerState samp, CloudShadowProjection p,
                                 float3 world_pos) {
    return map.SampleLevel(samp, cloud_shadow_uv(p, world_pos), 0.0).r;
}
