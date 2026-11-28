// grass_common.slang — shared by the grass compute and draw passes (R9.3).
//
// GrassParams mirrors engine::renderer::GrassParams (grass_pass.cpp), and the
// hash / LOD helpers mirror the CPU reference in engine/renderer/grass.hpp
// bit for bit where tests compare them.

// One frame's grass constants (uniform buffer, std140: float4-aligned only).
public struct GrassParams {
    public float4x4 view_proj;     // draw (TAA-jittered)
    public float4   planes[6];     // culling frustum (unjittered), normalised, inward
    public float4   camera;        // xyz camera position, w wind time (s)
    public int4     grid;          // xy world cell of grid cell (0,0), zw cells (x, z)
    public float4   cell;          // x cell size, y radius, z near-LOD distance, w thinning start
    public float4   lod;           // x far density, y edge band start (m), z max blade height, w max width scale
    public float4   blade;         // xy height range, zw width range (m)
    public float4   limits;        // xy ground normal.y range, zw ground height range
    public float4   wind;          // xy direction (XZ), z strength, w frequency (Hz)
    public float4   base_color;    // rgb linear, w terrain_only (0/1)
    public float4   tip_color;     // rgb linear, w flat ground height
    public uint4    counts;        // x seed, y height tiles, z near capacity, w far capacity
    public uint4    draw;          // x near index count, y far index count, z near segments, w far segments
    public float4   tile_rect[4];  // xy origin XZ, z size, w base y
    public float4   tile_info[4];  // x texel uv, y metres per texel
    public float4   ground;        // x min ground y, y max ground y (tile culling bounds)
    public float4   wind_rect;     // R13.1 wind texture: xy world XZ corner, z 1 / size m, w log-profile factor at blade height
    public float4   wind_flow;     // R13.1: xy mean heading (flutter phase), z 1 = wind field bound (else the R9.3 sway)
    public float4x4 prev_view_proj; // velocity (R11): last frame, unjittered
    public float4   motion;         // x previous wind time (s), yz this frame's jitter (NDC)
}

// A visible blade: world root + packed variation (bits 0-15 shape hash,
// 16-23 height fade unorm8, 24-31 width scale unorm8 over [1, max scale]).
public struct GrassBlade {
    public float3 root;
    public uint   packed;
}

// Indirect-argument buffer (uints): two VkDrawIndexedIndirectCommands, then
// the raw counters the finalize pass clamps and the frame's statistics (over
// the tiles that survive tile culling).
public static const uint ARGS_NEAR = 0;
public static const uint ARGS_FAR = 5;
public static const uint ARGS_NEAR_COUNT = 10;
public static const uint ARGS_FAR_COUNT = 11;
public static const uint ARGS_PLACED = 12;     // passed ground + LOD tests
public static const uint ARGS_CANDIDATES = 13; // cells within the radius

public static const float FADE_BAND = 0.15;

// pcg3d (Jarzynski & Olano 2020).
public uint3 pcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

public float3 unit_floats(uint3 h) {
    return float3(h >> 8u) * (1.0 / 16777216.0);
}

// Kept fraction at `distance` (grass_thinning / grass_keep_fraction).
public float grass_thinning(GrassParams p, float distance) {
    return lerp(1.0, p.lod.x, smoothstep(p.cell.w, p.cell.y, distance));
}

public float grass_keep(GrassParams p, float distance) {
    if (distance >= p.cell.y) return 0.0;
    return grass_thinning(p, distance) * (1.0 - smoothstep(p.lod.y, p.cell.y, distance));
}

public float grass_blade_fade(float keep, float threshold) {
    float t = threshold * (1.0 - FADE_BAND);
    if (t >= keep) return 0.0;
    return saturate((keep - t) / FADE_BAND);
}

// A blade's shape, derived from its 16-bit hash (both passes agree on it).
public struct BladeShape {
    public float  height;    // before fade
    public float  width;     // before width compensation
    public float2 facing;    // unit XZ normal of the blade's plane
    public float  lean;      // static bend (tip offset per metre of height)
    public float  variation; // albedo multiplier
    public float  phase;     // wind phase offset (radians)
}

public BladeShape blade_shape(GrassParams p, uint hash) {
    float3 u = unit_floats(pcg3d(uint3(hash, p.counts.x, 0x9E3779B9u)));
    float3 v = unit_floats(pcg3d(uint3(hash ^ 0xA511u, p.counts.x, 0x85EBCA6Bu)));
    BladeShape s;
    s.height = lerp(p.blade.x, p.blade.y, u.x * u.x * (3.0 - 2.0 * u.x));
    s.width = lerp(p.blade.z, p.blade.w, u.y);
    float angle = u.z * 6.28318531;
    s.facing = float2(cos(angle), sin(angle));
    s.lean = 0.05 + 0.35 * v.x;
    s.variation = 0.82 + 0.36 * v.y;
    s.phase = v.z * 6.28318531;
    return s;
}

public uint pack_blade(uint hash, float fade, float width_scale, float max_width_scale) {
    uint f = uint(saturate(fade) * 255.0 + 0.5);
    uint w = uint(saturate((width_scale - 1.0) / max(max_width_scale - 1.0, 1e-3)) * 255.0 + 0.5);
    return (hash & 0xFFFFu) | (f << 16u) | (w << 24u);
}

public float blade_fade(uint packed) { return float((packed >> 16u) & 0xFFu) / 255.0; }

public float blade_width_scale(uint packed, float max_width_scale) {
    return 1.0 + float(packed >> 24u) / 255.0 * (max_width_scale - 1.0);
}
