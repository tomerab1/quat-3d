// terrain_edit.slang — terrain edit stamps on the GPU (ROADMAP G19.1).
//
// Applies an ordered stamp list (engine/terrain/edits.hpp) to one tile's
// height field and paint, inside an R10.2 synthesis job (TerrainEditPass):
//
//   edit_init       g_base = g_heights (kept for later CPU recomputes),
//                   g_paint = no material, full grass
//   edit_pointwise  stamps [first, first + count), none of them smooth, in
//                   order per texel: flatten / raise / lower / paint
//   edit_copy       g_scratch = g_heights (one smooth pass's input)
//   edit_smooth     stamp `first`: 3x3 binomial blur of g_scratch blended by
//                   coverage x strength (tile-border texels excluded)
//
// The arithmetic mirrors src/terrain/edits.cpp and edits_apply.cpp operation
// for operation; per-edge reciprocals and plane slopes arrive precomputed
// (PackedStamps), so neither side divides where the other multiplies. GPU
// sqrt and fused multiply-adds differ from the CPU by an ULP or two, so the
// result matches the CPU reference within a tolerance
// (tests/renderer/terrain_edit_gpu_test.cpp). Distances are Quilez's 2D
// signed distance functions (circle, segment, sdPolygon).

// Mirrors terrain::PackedStamp (96 bytes, std430).
struct Stamp {
    uint   op;     // StampOp: flatten 0, raise 1, lower 2, smooth 3, paint 4
    uint   shape;  // StampShape: circle 0, capsule 1, polygon 2
    uint   first;  // first point / edge
    uint   count;
    float  radius;
    float  falloff;
    float  inv_falloff;
    float  strength;
    float  amount; // signed
    float  height;
    float2 pivot;
    float2 slope;
    uint   channel;
    float  value;
    uint   iterations;
    uint   pad0;
    uint   pad1;
    uint   pad2;
    float4 bounds; // min xz, max xz
};

struct Push {
    int   tile_x;    // world tile coordinate
    int   tile_z;
    uint  res;       // texels per side
    float texel_m;   // tile_size / (res - 1)
    float half_size; // tile_size / 2
    uint  first;     // first stamp (edit_pointwise), the stamp (edit_smooth)
    uint  count;     // stamps (edit_pointwise)
};
[[vk::push_constant]] Push pc;

StructuredBuffer<Stamp>   g_stamps;
StructuredBuffer<float2>  g_points;
StructuredBuffer<float4>  g_edges;   // xy = edge vector, z = 1 / dot(edge, edge)
RWStructuredBuffer<float> g_heights; // the tile, metres
RWStructuredBuffer<float> g_base;    // the generated heights
RWStructuredBuffer<float> g_scratch; // smooth input
RWStructuredBuffer<uint>  g_paint;   // RGBA8: dirt, gravel, soil, grass mask

static const uint OP_FLATTEN = 0;
static const uint OP_SMOOTH = 3;
static const uint OP_PAINT = 4;
static const uint SHAPE_CIRCLE = 0;
static const uint SHAPE_CAPSULE = 1;
static const uint PAINT_DEFAULT = 0xFF000000u;

bool inside(uint3 id) { return id.x < pc.res && id.y < pc.res; }
uint index_of(uint x, uint z) { return z * pc.res + x; }

// TileFrame::texel_position: the world texel lattice index times the texel.
float2 texel_position(uint x, uint z) {
    const int span = int(pc.res) - 1;
    const int gx = pc.tile_x * span + int(x);
    const int gz = pc.tile_z * span + int(z);
    return float2(float(gx) * pc.texel_m - pc.half_size, float(gz) * pc.texel_m - pc.half_size);
}

float lerp_ref(float x, float y, float a) { return x * (1.0 - a) + y * a; }

bool in_bounds(Stamp s, float2 p) {
    return !(p.x < s.bounds.x || p.y < s.bounds.y || p.x > s.bounds.z || p.y > s.bounds.w);
}

// terrain::packed_coverage.
float coverage(Stamp s, float2 p) {
    float d = 0.0;
    if (s.shape == SHAPE_CIRCLE) {
        d = length(p - g_points[s.first]);
    } else if (s.shape == SHAPE_CAPSULE) {
        const float2 w0 = p - g_points[s.first];
        float best = dot(w0, w0);
        for (uint i = 0; i + 1 < s.count; ++i) {
            const float4 e4 = g_edges[s.first + i];
            const float2 e = e4.xy;
            const float2 w = p - g_points[s.first + i];
            const float h = clamp(dot(w, e) * e4.z, 0.0, 1.0);
            const float2 b = w - e * h;
            best = min(best, dot(b, b));
        }
        d = sqrt(best);
    } else {
        const float2 w0 = p - g_points[s.first];
        float best = dot(w0, w0);
        float sgn = 1.0;
        for (uint i = 0; i < s.count; ++i) {
            const float4 e4 = g_edges[s.first + i];
            const float2 e = e4.xy; // v[i - 1] - v[i]
            const float2 v = g_points[s.first + i];
            const float2 w = p - v;
            const float h = clamp(dot(w, e) * e4.z, 0.0, 1.0);
            const float2 b = w - e * h;
            best = min(best, dot(b, b));
            const float2 prev = v + e;
            const bool c0 = p.y >= v.y;
            const bool c1 = p.y < prev.y;
            const bool c2 = e.x * w.y > e.y * w.x;
            if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) sgn = -sgn;
        }
        d = sgn * sqrt(best);
    }
    d -= s.radius;
    if (d <= 0.0) return 1.0;
    if (s.inv_falloff == 0.0) return 0.0;
    const float t = min(d * s.inv_falloff, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void edit_init(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    const uint i = index_of(id.x, id.y);
    g_base[i] = g_heights[i];
    g_paint[i] = PAINT_DEFAULT;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void edit_pointwise(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    const uint i = index_of(id.x, id.y);
    const float2 p = texel_position(id.x, id.y);
    float h = g_heights[i];
    uint paint = g_paint[i];
    for (uint k = 0; k < pc.count; ++k) {
        const Stamp s = g_stamps[pc.first + k];
        if (!in_bounds(s, p)) continue;
        const float c = coverage(s, p);
        if (c <= 0.0) continue;
        const float a = c * s.strength;
        if (s.op == OP_FLATTEN) {
            const float target =
                s.height + s.slope.x * (p.x - s.pivot.x) + s.slope.y * (p.y - s.pivot.y);
            h = lerp_ref(h, target, a);
        } else if (s.op == OP_PAINT) {
            const uint shift = 8u * s.channel;
            const float current = float((paint >> shift) & 0xFFu) * (1.0 / 255.0);
            const float v = lerp_ref(current, s.value, a);
            const uint byte = uint(clamp(v * 255.0 + 0.5, 0.0, 255.0));
            paint = (paint & ~(0xFFu << shift)) | (byte << shift);
        } else { // raise / lower (smooth stamps never reach this kernel)
            h = h + s.amount * a;
        }
    }
    g_heights[i] = h;
    g_paint[i] = paint;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void edit_copy(uint3 id : SV_DispatchThreadID) {
    if (!inside(id)) return;
    const uint i = index_of(id.x, id.y);
    g_scratch[i] = g_heights[i];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void edit_smooth(uint3 id : SV_DispatchThreadID) {
    // Tile-border texels are never smoothed: neighbouring tiles share them.
    if (id.x < 1 || id.y < 1 || id.x + 1 >= pc.res || id.y + 1 >= pc.res) return;
    const Stamp s = g_stamps[pc.first];
    const float2 p = texel_position(id.x, id.y);
    if (!in_bounds(s, p)) return;
    const float c = coverage(s, p);
    if (c <= 0.0) return;
    const uint x = id.x;
    const uint z = id.y;
    const float row0 = g_scratch[index_of(x - 1, z - 1)] + 2.0 * g_scratch[index_of(x, z - 1)] +
                       g_scratch[index_of(x + 1, z - 1)];
    const float row1 = g_scratch[index_of(x - 1, z)] + 2.0 * g_scratch[index_of(x, z)] +
                       g_scratch[index_of(x + 1, z)];
    const float row2 = g_scratch[index_of(x - 1, z + 1)] + 2.0 * g_scratch[index_of(x, z + 1)] +
                       g_scratch[index_of(x + 1, z + 1)];
    const float blur = (row0 + 2.0 * row1 + row2) * 0.0625;
    const uint i = index_of(x, z);
    g_heights[i] = lerp_ref(g_scratch[i], blur, c * s.strength);
}
