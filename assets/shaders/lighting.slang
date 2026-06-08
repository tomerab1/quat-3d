// lighting.slang — deferred lighting pass (Phase 3, Slice 3.6).
//
// A fullscreen compute pass: reads the GBuffer (albedo, octahedral world normal,
// packed metallic/roughness/occlusion) and applies a single directional light
// plus a flat ambient term, writing linear HDR colour. View-dependent specular
// arrives once a camera exists (Phase 4); this slice is Lambert diffuse +
// occluded ambient. The material target's alpha is the coverage mask (1 where
// geometry was rasterised, 0 for background).

// Light parameters. Packed into vec4s for std430 push-constant friendliness.
struct LightParams {
    float4 direction;   // xyz = direction TO the light (normalised); w unused
    float4 color;       // rgb = colour, w = intensity
    float4 ambient;     // rgb = ambient colour, w unused
}
[[vk::push_constant]] LightParams pc;

[[vk::binding(0)]] Texture2D          g_albedo;
[[vk::binding(1)]] Texture2D          g_normal;
[[vk::binding(2)]] Texture2D          g_material;
[[vk::binding(3)]] RWTexture2D<float4> g_hdr;

// Octahedral decode (inverse of gbuffer.slang's oct_encode).
float3 oct_decode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_hdr.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) {
        return;
    }
    int2 coord = int2(tid.xy);

    float4 albedo = g_albedo.Load(int3(coord, 0));
    float4 mat    = g_material.Load(int3(coord, 0));
    float2 enc    = g_normal.Load(int3(coord, 0)).xy;

    float coverage  = mat.a;            // 1 = geometry, 0 = background
    float metallic  = mat.r;
    float occlusion = mat.b;

    float3 n = oct_decode(enc);
    float3 l = normalize(pc.direction.xyz);
    float  ndotl = max(dot(n, l), 0.0);

    float3 diffuse = albedo.rgb * (1.0 - metallic);
    float3 lit     = diffuse * pc.color.rgb * pc.color.w * ndotl;
    float3 ambient = pc.ambient.rgb * albedo.rgb * occlusion;

    float3 color = (lit + ambient) * coverage;
    g_hdr[coord] = float4(color, 1.0);
}
