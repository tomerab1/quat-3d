// grass.slang — grass blades into the deferred GBuffer (R9.3).
//
// No vertex buffer: each instance is one GrassBlade from the compute pass's
// list (drawn with vkCmdDrawIndexedIndirect, firstInstance = the list's
// offset), and the vertex id walks the blade strip: a vertex pair per level
// (left / right edge) and one tip vertex. The blade is a quadratic Bezier
// from the root to a tip bent by the blade's static lean plus the wind:
// with weather, the global wind field texture (R13.1: drag grows with the
// square of the wind speed at blade height and saturates, plus a per-blade
// flutter); without, the original scrolling gust noise (R9.3),
// with a tapering width. Blades seen edge-on turn slightly toward the
// camera so the field does not thin out at grazing angles. Normals are
// rounded across the blade's width and pulled toward the ground's up
// direction for soft, ground-cover-like shading. Opaque (no alpha test or
// blending), so grass stays in the deferred path.

import grass_common;
import instancing; // instance_index(): InstanceIndex including firstInstance
import wind_common;

[[vk::binding(0)]] ConstantBuffer<GrassParams>  g_params;
[[vk::binding(1)]] StructuredBuffer<GrassBlade> g_blades;
[[vk::binding(2)]] Texture2D                    g_wind;         // WindFieldPass output
[[vk::binding(3)]] SamplerState                 g_wind_sampler; // linear clamp

// Mirrors engine::renderer GrassDrawPush (16 bytes).
struct PushConstants {
    uint  segments; // levels of this LOD's strip
    uint  base;     // first blade of this LOD's region (firstInstance is always 0)
    uint2 pad;
}
[[vk::push_constant]] PushConstants pc;

struct VsOut {
    float4 clip   : SV_Position;
    float3 world  : WORLD_POS;
    float3 normal : NORMAL;   // blade face normal (unrounded)
    float3 side   : TANGENT;  // across the blade
    float2 coord  : TEXCOORD0; // x across width [-1, 1], y along the blade [0, 1]
    nointerpolation float variation : TEXCOORD1;
}

// ---- Without weather: the original scrolling-gust sway (R9.3) ------------
// Scenes without a weather entity have no wind field (WindFieldPass does not
// run) and keep exactly this look (wind_flow.z = 0).

float wind_hash(float2 p) {
    float3 q = frac(float3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, float3(q.y, q.z, q.x) + 33.33);
    return frac((q.x + q.y) * q.z);
}

float wind_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 w = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(wind_hash(i), wind_hash(i + float2(1, 0)), w.x),
                lerp(wind_hash(i + float2(0, 1)), wind_hash(i + float2(1, 1)), w.x), w.y);
}

float2 legacy_wind_offset(float3 root, float phase, float variation) {
    const float2 dir = g_params.wind.xy;
    const float strength = g_params.wind.z;
    const float freq = g_params.wind.w;
    const float time = g_params.camera.w;
    // Gusts: two octaves of value noise scrolling downwind (~16 m and ~6 m waves).
    const float2 q = root.xz * 0.06 - dir * time * freq;
    const float gust = 0.65 * wind_noise(q) + 0.35 * wind_noise(q * 2.7 + 13.1);
    // Flutter: each blade oscillates around the gust-bent pose.
    const float flutter =
        sin(time * freq * 6.28318531 * (2.2 + 0.6 * variation) + phase + dot(root.xz, dir) * 0.8);
    const float bend = strength * (0.15 + 1.1 * gust * gust) + strength * 0.12 * flutter;
    return dir * bend;
}

// ---- With weather: the global wind field (R13.1) --------------------------
// Blade response: (wind speed at blade height / this)^2 is the
// drag-to-stiffness ratio q; the tip offset per metre is strength * 2.2 *
// q / (1 + q) (drag grows with the square of the speed, bending saturates).
static const float BLADE_STIFF_SPEED = 3.0; // m/s at blade height

float2 field_wind_offset(float3 root, float phase, float variation) {
    const float strength = g_params.wind.z;
    const float freq = g_params.wind.w;
    const float time = g_params.camera.w;
    // The global wind field at the root, scaled from its 10 m reference
    // height down to the blades (log profile).
    const float4 field = wind_texture_sample(g_wind, g_wind_sampler, g_params.wind_rect, root.xz);
    const float2 v = field.xy * g_params.wind_rect.w;
    const float speed = length(v);
    if (speed < 1e-4) return float2(0.0, 0.0);
    const float2 dir = v / speed; // local: bends the blade
    const float q = (speed * speed) / (BLADE_STIFF_SPEED * BLADE_STIFF_SPEED);
    const float bend = strength * 2.2 * q / (1.0 + q);
    // Flutter: each blade oscillates around the bent pose, harder in gusts.
    // The travelling wave's phase follows the MEAN heading, which is the
    // same everywhere and changes only when the weather does: the local
    // direction turns with the turbulence, and dot(root, local dir) far
    // from the origin would jump by many radians per frame.
    const float flutter = sin(time * freq * 6.28318531 * (2.2 + 0.6 * variation) + phase +
                              dot(root.xz, g_params.wind_flow.xy) * 0.8);
    const float gust = saturate(field.w - 0.6);
    return dir * (bend + strength * 0.22 * saturate(q) * (0.5 + gust) * flutter);
}

// Horizontal tip displacement per metre of blade height from the wind.
float2 wind_offset(float3 root, float phase, float variation) {
    return g_params.wind_flow.z > 0.5 ? field_wind_offset(root, phase, variation)
                                      : legacy_wind_offset(root, phase, variation);
}

[shader("vertex")]
VsOut vertex_main(uint vertex_id : SV_VertexID) {
    const GrassBlade blade = g_blades[pc.base + instance_index()];
    const BladeShape shape = blade_shape(g_params, blade.packed & 0xFFFFu);
    const float height = shape.height * blade_fade(blade.packed);
    const float width = shape.width * blade_width_scale(blade.packed, g_params.lod.w);
    const float3 up = float3(0.0, 1.0, 0.0);
    const float3 root = blade.root;

    // Edge-on blades turn up to ~30 degrees toward the camera.
    float2 facing = shape.facing;
    const float2 to_cam = g_params.camera.xz - root.xz;
    const float cam_dist = length(to_cam);
    if (cam_dist > 1e-4) {
        const float2 v = to_cam / cam_dist;
        const float d = dot(facing, v);
        facing = normalize(facing + v * (d >= 0.0 ? 1.0 : -1.0) * (1.0 - abs(d)) * 0.6);
    }
    const float3 side = float3(-facing.y, 0.0, facing.x);

    // Tip direction: static lean along the facing plus the wind; the control
    // point drops as the blade bends so its length stays roughly constant.
    const float2 offset = facing * shape.lean + wind_offset(root, shape.phase, shape.variation);
    const float3 tip_dir = normalize(float3(offset.x, 1.0, offset.y));
    const float3 p0 = root;
    const float3 p1 = root + up * (height * 0.62 * tip_dir.y);
    const float3 p2 = root + tip_dir * height;

    const uint segments = pc.segments;
    const uint level = min(vertex_id / 2, segments);
    const float across = level == segments ? 0.0 : ((vertex_id & 1u) == 0u ? -1.0 : 1.0);
    const float t = float(level) / float(segments);
    const float s = 1.0 - t;
    const float3 centre = s * s * p0 + 2.0 * s * t * p1 + t * t * p2;
    const float3 tangent = normalize(2.0 * s * (p1 - p0) + 2.0 * t * (p2 - p1) + up * 1e-4);
    const float half_width = 0.5 * width * pow(max(s, 0.0), 0.65);

    VsOut o;
    o.world = centre + side * (across * half_width);
    o.clip = mul(g_params.view_proj, float4(o.world, 1.0));
    o.normal = normalize(cross(side, tangent));
    o.side = side;
    o.coord = float2(across, t);
    o.variation = shape.variation;
    return o;
}

float2 sign_nonzero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 oct_encode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(float2(e.y, e.x))) * sign_nonzero(e);
    }
    return e;
}

struct GBufferOut {
    float4 albedo    : SV_Target0;
    float2 normal    : SV_Target1;
    float4 material  : SV_Target2; // r metallic, g roughness, b occlusion, a coverage
    float2 clearcoat : SV_Target3;
}

[shader("fragment")]
GBufferOut fragment_main(VsOut input) {
    // Two-sided: the face normal points at the viewer, then rounds across
    // the width and leans toward up (soft, ground-cover shading).
    const float3 view = normalize(g_params.camera.xyz - input.world);
    float3 n = normalize(input.normal);
    if (dot(n, view) < 0.0) n = -n;
    n = normalize(n + normalize(input.side) * (input.coord.x * 0.55));
    n = normalize(lerp(n, float3(0.0, 1.0, 0.0), 0.35));

    const float t = saturate(input.coord.y);
    const float3 albedo =
        lerp(g_params.base_color.rgb, g_params.tip_color.rgb, smoothstep(0.0, 1.0, t)) *
        input.variation;
    // Cheap self-occlusion: the bottom of the field sits in the blades' shade.
    const float occlusion = lerp(0.3, 1.0, sqrt(t));

    GBufferOut o;
    o.albedo = float4(saturate(albedo), 1.0);
    o.normal = oct_encode(n);
    o.material = float4(0.0, 0.62, occlusion, 1.0);
    o.clearcoat = float2(0.0, 0.0);
    return o;
}
