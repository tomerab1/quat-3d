// particle_draw.slang — camera-facing particle quads (ROADMAP R10.3).
//
// One indexed instance per drawn particle (vkCmdDrawIndexedIndirect with the
// simulation's arguments); the vertex shader builds the rotated quad from the
// particle, evaluates the size / colour curves and the flipbook frames, and —
// for lit emitters — lights the corner with the sun, the ambient term and the
// clustered point / spot lights of its froxel (R10.1), treating the particle
// as a small isotropic volume (no N.L). The fragment shader blends two
// flipbook frames and fades the particle out near the scene depth (soft
// particles), then writes either
//   fragment_additive — premultiplied light, blended ONE, ONE into the HDR, or
//   fragment_wboit    — weighted blended OIT (McGuire & Bavoil, JCGT 2013):
//                       accumulation (RGBA16F, ONE, ONE) and revealage
//                       (R16F, ZERO, ONE_MINUS_SRC_COLOR) for the composite.
// Depth is tested read-only against the scene, so particles hide behind
// geometry and never occlude each other.

import particle_common;
import clustered_lights;
import instancing; // instance_index(): Vulkan InstanceIndex (no DrawParameters)

ConstantBuffer<ParticleFrame> g_frame;
StructuredBuffer<Emitter>     g_emitters;
StructuredBuffer<Particle>    g_particles;
StructuredBuffer<uint>        g_alive;
Texture2D<float>              g_depth;   // scene depth (read-only attachment too)
Texture2D                     g_texture; // flipbook atlas
SamplerState                  g_sampler;
ConstantBuffer<ClusterParams> g_cluster_params;
StructuredBuffer<GpuLight>    g_lights;
StructuredBuffer<uint2>       g_cluster_grid;
StructuredBuffer<uint>        g_cluster_indices;

// Mirrors engine::renderer ParticlePush (particle_pass.cpp).
struct PushConstants {
    uint emitter;
    uint k;
    uint j;
    uint pad;
}
[[vk::push_constant]] PushConstants pc;

static const float INV_PI = 0.31830988618;

struct VsOut {
    float4 position : SV_Position;
    float4 uv : TEXCOORD0;                     // flipbook frame a (xy), frame b (zw)
    float4 color : COLOR0;                     // rgb light (premultiplied by nothing), a alpha
    float  view_depth : TEXCOORD1;
    nointerpolation float frame_blend : TEXCOORD2;
}

// Light arriving at a world position from the clustered lights.
float3 clustered_radiance(float3 world_pos) {
    float3 sum = float3(0.0, 0.0, 0.0);
    if (g_cluster_params.dims.w == 0) {
        return sum;
    }
    uint2 range = g_cluster_grid[cluster_index_of_world(g_cluster_params, world_pos)];
    for (uint k = 0; k < range.y; ++k) {
        GpuLight light = g_lights[g_cluster_indices[range.x + k]];
        float3 l;
        float3 radiance;
        if (light_incident(light, world_pos, l, radiance)) {
            sum += radiance;
        }
    }
    return sum;
}

float2 flipbook_cell(Emitter e, uint frame) {
    uint columns = uint(e.flipbook.x);
    return float2(float(frame % columns), float(frame / columns));
}

[shader("vertex")]
VsOut vertex_main(uint vertex_id : SV_VertexID) {
    Emitter e = g_emitters[pc.emitter];
    uint slot = g_alive[g_frame.misc.z * g_frame.misc.y + e.range.x + instance_index()];
    Particle p = g_particles[slot];

    float t = saturate(p.age / p.lifetime);
    float size = p.size * particle_size_at(e, t);
    float4 color = particle_color_at(e, t);

    float2 corner = float2(float(vertex_id & 1u), float(vertex_id >> 1u)) - 0.5;
    float s = sin(p.rotation);
    float c = cos(p.rotation);
    float2 r = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c) * size;
    float3 centre = particle_world_position(e, p.position);
    float3 world_pos = centre + g_frame.camera_right.xyz * r.x + g_frame.camera_up.xyz * r.y;

    VsOut o;
    o.position = particle_mul(g_frame.view_proj, float4(world_pos, 1.0));
    o.view_depth = particle_view_depth(g_frame, world_pos);

    // Flipbook: `fps` loops through the frames; fps 0 plays them once over the life.
    uint frames = max(uint(e.flipbook.z), 1u);
    float phase = e.flipbook.w > 0.0 ? p.age * e.flipbook.w : t * float(frames);
    float whole = floor(phase);
    uint f0;
    uint f1;
    if (e.flipbook.w > 0.0) {
        f0 = uint(whole) % frames;
        f1 = (f0 + 1) % frames;
    } else {
        f0 = min(uint(whole), frames - 1);
        f1 = min(f0 + 1, frames - 1);
    }
    o.frame_blend = phase - whole;
    float2 quad_uv = float2(corner.x + 0.5, 0.5 - corner.y);
    float2 cell_size = 1.0 / e.flipbook.xy;
    o.uv = float4((flipbook_cell(e, f0) + quad_uv) * cell_size,
                  (flipbook_cell(e, f1) + quad_uv) * cell_size);

    float3 rgb = color.rgb;
    if ((e.config.y & PARTICLE_FLAG_LIT) != 0) {
        float3 light = g_frame.sun_color.rgb + clustered_radiance(world_pos);
        rgb *= g_frame.ambient.rgb + light * INV_PI;
    } else {
        rgb *= e.render.y;
    }
    o.color = float4(rgb, color.a);
    return o;
}

// Shaded colour (rgb, not premultiplied) and coverage of a fragment.
float4 shade(VsOut input) {
    Emitter e = g_emitters[pc.emitter];
    float4 a = g_texture.Sample(g_sampler, input.uv.xy);
    float4 b = g_texture.Sample(g_sampler, input.uv.zw);
    float4 tex = lerp(a, b, input.frame_blend);
    float alpha = input.color.a * tex.a;
    if (e.render.x > 0.0) {
        float2 px = input.position.xy;
        float scene = g_depth.Load(int3(int2(px), 0));
        float2 ndc = px * g_frame.screen.zw * 2.0 - 1.0;
        float scene_depth = particle_linear_depth(g_frame, ndc, scene);
        alpha *= saturate((scene_depth - input.view_depth) * e.render.x);
    }
    return float4(input.color.rgb * tex.rgb, alpha);
}

[shader("fragment")]
float4 fragment_additive(VsOut input) : SV_Target0 {
    float4 s = shade(input);
    return float4(s.rgb * s.a, 0.0);
}

struct WboitOut {
    float4 accum : SV_Target0;
    float4 reveal : SV_Target1;
}

[shader("fragment")]
WboitOut fragment_wboit(VsOut input) {
    float4 s = shade(input);
    float z = input.view_depth / 5.0;
    float far = input.view_depth / 200.0;
    float far2 = far * far;
    float w = s.a * clamp(10.0 / (1e-5 + z * z + far2 * far2 * far2), 1e-2, 3e3);
    WboitOut o;
    o.accum = float4(s.rgb * s.a, s.a) * w;
    o.reveal = float4(s.a, 0.0, 0.0, 0.0);
    return o;
}
