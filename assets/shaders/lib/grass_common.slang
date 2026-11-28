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
    public float4   ground;        // x min ground y, y max ground y (tile culling bounds), z slope fade start normal.y, w ground cover fade start (m, XZ)
    public float4   wind_rect;     // R13.1 wind texture: xy world XZ corner, z 1 / size m, w log-profile factor at blade height
    public float4   wind_flow;     // R13.1: xy mean heading (flutter phase), z 1 = wind field bound (else the R9.3 sway)
    public float4x4 prev_view_proj; // velocity (R11): last frame, unjittered
    public float4   motion;         // x previous wind time (s), yz this frame's jitter (NDC)
    // Grass v2.
    public float4   clump;          // x clump size (0 = off), y pull, z max pull (m), w ground cover fade end (m, XZ; < 0: unbounded)
    public float4   look;           // x colour variation, y dry fraction, z root tint, w 1 = GBuffer bound (root tint)
    public float4   dry_color;      // rgb dry tip colour, w min blade width (px)
    public float4   footprint;      // x pixels per metre at 1 m (0 = off), y mean blade width, z ground cover near, w far
    public float4   cover_color;    // rgb thatch colour (ground cover near the camera)
    public float4   far_color;      // rgb the blades' mean colour (ground cover at the radius)
    public float4x4 inv_view_proj;  // inverse of view_proj (GBuffer depth -> world)
    public float4   screen;         // xy render extent (px), zw 1 / extent
}

// A visible blade: world root + packed variation (bits 0-15 shape hash,
// 16-23 height fade unorm8, 24-31 width scale unorm8 over [1, max scale]),
// its clump's hash, and the ground's albedo at the root (rgb unorm8 of the
// square root, a = weight; 0 where the root was not visible this frame).
public struct GrassBlade {
    public float3 root;
    public uint   packed;
    public uint   clump;
    public uint   tint;
    public uint2  reserved;
}

public static const uint CLUMP_SALT = 0x5BD1E995u; // engine::renderer::grass_clump_salt
public static const float MIN_FOOTPRINT_KEEP = 0.05;
public static const float CLUMP_MAX_HEIGHT_SCALE = 1.2; // grass_clump_max_height_scale

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

// Slope fade (grass_slope_keep): full density at normal.y >= ground.z,
// thinning to 0 at the steepest allowed slope (limits.x).
public float grass_slope_keep(GrassParams p, float normal_y) {
    if (p.ground.z <= -1.5) return 1.0;
    if (p.ground.z <= p.limits.x) return normal_y >= p.limits.x ? 1.0 : 0.0;
    return smoothstep(p.limits.x, p.ground.z, normal_y);
}

// Pixel-footprint thinning (grass_footprint_keep): blades whose projected
// width falls below the minimum thin out (survivors widen to keep coverage).
public float grass_footprint_keep(GrassParams p, float distance) {
    if (p.footprint.x <= 0.0 || p.dry_color.w <= 0.0 || distance <= 0.0) return 1.0;
    const float px = p.footprint.y * p.footprint.x / distance;
    return clamp(px / p.dry_color.w, MIN_FOOTPRINT_KEEP, 1.0);
}

// Width multiplier for a cell thinned to `thinning` (grass_width_scale).
public float grass_width_scale(GrassParams p, float thinning) {
    return min(1.0 / max(thinning, 1e-4), p.lod.w);
}

// The clump `xz` belongs to (grass_clump): the nearest jittered centre of
// the 3x3 clump cells around it (Worley, "A Cellular Texture Basis
// Function", SIGGRAPH 1996).
public struct Clump {
    public float2 centre;
    public uint   hash;
}

public Clump grass_clump(GrassParams p, float2 xz) {
    Clump out_clump;
    out_clump.centre = xz;
    out_clump.hash = 0;
    const float size = p.clump.x;
    if (size <= 0.0) return out_clump;
    const int2 home = int2(floor(xz / size));
    float best = 3.402823466e38;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 c = home + int2(dx, dz);
            const uint3 h = pcg3d(uint3(uint(c.x), uint(c.y), p.counts.x ^ CLUMP_SALT));
            const float2 u = unit_floats(h).xy;
            const float2 centre = (float2(c) + u) * size;
            const float2 d = centre - xz;
            const float d2 = d.x * d.x + d.y * d.y;
            if (d2 < best) {
                best = d2;
                out_clump.centre = centre;
                out_clump.hash = h.z;
            }
        }
    }
    return out_clump;
}

// Ground height + normal at world XZ from up to four terrain height tiles
// (the terrain pass's uv convention and central-difference normal), else the
// flat ground. False where there is no ground to grow on.
float sample_height_tile(Texture2D h0, Texture2D h1, Texture2D h2, Texture2D h3,
                         SamplerState s, uint tile, float2 uv) {
    switch (tile) {
    case 0: return h0.SampleLevel(s, uv, 0.0).r;
    case 1: return h1.SampleLevel(s, uv, 0.0).r;
    case 2: return h2.SampleLevel(s, uv, 0.0).r;
    default: return h3.SampleLevel(s, uv, 0.0).r;
    }
}

public bool grass_ground_at(GrassParams p, Texture2D h0, Texture2D h1, Texture2D h2,
                            Texture2D h3, SamplerState s, float2 xz, out float height,
                            out float3 normal) {
    for (uint k = 0; k < p.counts.y; ++k) {
        const float4 rect = p.tile_rect[k];
        const float2 local = xz - rect.xy;
        if (all(local >= 0.0) && all(local <= rect.z)) {
            const float2 uv = local / rect.z;
            const float du = p.tile_info[k].x;
            const float mpt = p.tile_info[k].y;
            height = rect.w + sample_height_tile(h0, h1, h2, h3, s, k, uv);
            const float hl = sample_height_tile(h0, h1, h2, h3, s, k, uv - float2(du, 0.0));
            const float hr = sample_height_tile(h0, h1, h2, h3, s, k, uv + float2(du, 0.0));
            const float hd = sample_height_tile(h0, h1, h2, h3, s, k, uv - float2(0.0, du));
            const float hu = sample_height_tile(h0, h1, h2, h3, s, k, uv + float2(0.0, du));
            normal = normalize(float3(hl - hr, 2.0 * mpt, hd - hu));
            return true;
        }
    }
    height = p.tip_color.w;
    normal = float3(0.0, 1.0, 0.0);
    return p.base_color.w < 0.5;
}

// The slope / height limits of where grass grows (shared by blades and the
// ground cover).
public bool grass_ground_allowed(GrassParams p, float height, float3 normal) {
    return normal.y >= p.limits.x && normal.y <= p.limits.y && height >= p.limits.z &&
           height <= p.limits.w;
}

public float grass_blade_fade(float keep, float threshold) {
    float t = threshold * (1.0 - FADE_BAND);
    if (t >= keep) return 0.0;
    return saturate((keep - t) / FADE_BAND);
}

// A blade's shape, derived from its 16-bit hash and its clump's hash (both
// passes agree on it). Clump traits after Ghost of Tsushima (Wohllaib, GDC
// 2021): blades of a clump share a height scale, a facing and a lean
// direction, and a dryness; each blade still varies around them.
public struct BladeShape {
    public float  height;    // before fade
    public float  width;     // before width compensation
    public float2 facing;    // unit XZ normal of the blade's plane
    public float2 lean_dir;  // unit XZ direction the blade leans / curls toward
    public float  lean;      // static tilt (tip offset per metre of height)
    public float  bend;      // curvature: control point offset per metre of height
    public float  variation; // albedo value multiplier
    public float  hue;       // [-1, 1] hue shift (yellow <-> blue-green)
    public float  dryness;   // [0, 1] blend toward the dry colours
    public float  phase;     // wind phase offset (radians)
    public float  flutter;   // flutter frequency factor (0.82 - 1.18)
}

public BladeShape blade_shape(GrassParams p, uint hash, uint clump_hash) {
    const float3 u = unit_floats(pcg3d(uint3(hash, p.counts.x, 0x9E3779B9u)));
    const float3 v = unit_floats(pcg3d(uint3(hash ^ 0xA511u, p.counts.x, 0x85EBCA6Bu)));
    const float3 w = unit_floats(pcg3d(uint3(hash ^ 0x3C6Fu, p.counts.x, 0xC2B2AE35u)));
    const bool clumped = p.clump.x > 0.0;
    const float3 c = clumped ? unit_floats(pcg3d(uint3(clump_hash, p.counts.x, 0x27D4EB2Fu)))
                             : float3(0.5, 0.5, 0.5);
    BladeShape s;
    // Height: the blade's own spread around its clump's scale.
    const float own = u.x * u.x * (3.0 - 2.0 * u.x);
    const float clump_scale = clumped ? lerp(0.65, CLUMP_MAX_HEIGHT_SCALE, c.x) : 1.0;
    s.height = lerp(p.blade.x, p.blade.y, own) * clump_scale;
    s.width = lerp(p.blade.z, p.blade.w, u.y);
    // Facing: a clump's blades share a direction (+-40 degrees around it).
    const float clump_angle = c.y * 6.28318531;
    const float angle = clumped ? clump_angle + (u.z - 0.5) * 1.4 : u.z * 6.28318531;
    s.facing = float2(cos(angle), sin(angle));
    const float lean_angle = clumped ? clump_angle + (w.x - 0.5) * 1.2 : angle;
    s.lean_dir = float2(cos(lean_angle), sin(lean_angle));
    s.lean = 0.08 + 0.42 * v.x * v.x;
    s.bend = 0.10 + 0.30 * w.y;
    s.variation = lerp(1.0, 0.72 + 0.56 * v.y, p.look.x);
    s.hue = (w.z * 2.0 - 1.0) * p.look.x;
    // Dryness: whole clumps turn (dry_fraction of them), with a little per blade.
    const float dry_clump = (clumped ? c.z : w.x) < p.look.y ? 1.0 : 0.0;
    s.dryness = saturate(dry_clump * (0.55 + 0.45 * v.z) + 0.2 * p.look.x * v.z * v.z);
    s.phase = v.z * 6.28318531;
    s.flutter = 0.82 + 0.36 * v.y;
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
