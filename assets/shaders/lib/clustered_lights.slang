// clustered_lights.slang — clustered light records + lookup (ROADMAP R10.1).
//
// Shared by the light-cull compute pass (light_cull.slang), the deferred
// lighting pass and the forward transparent pass. Layouts mirror
// engine/renderer/light_clusters.hpp, which also holds the CPU reference of
// the cluster math below (keep the two in step).
//
// The view frustum is split into dims.x × dims.y NDC tiles × dims.z depth
// slices, exponentially spaced between near and far. Matrices are stored as
// four column vectors (glm column-major) and applied with cluster_mul, so the
// math does not depend on a matrix layout qualifier.

public static const uint LIGHT_TYPE_POINT = 0;
public static const uint LIGHT_TYPE_SPOT  = 1;

// One light (std430, 64 bytes; engine::renderer::GpuLight).
public struct GpuLight {
    public float4 position_range;    // xyz world position, w range (culling radius)
    public float4 color_intensity;   // rgb colour, w intensity
    public float3 direction;         // spot axis, world space (unused for points)
    public uint   type;              // LIGHT_TYPE_*
    public float4 spot_scale_offset; // KHR_lights_punctual cone: x scale, y offset
}

// Per-frame cluster parameters (std140, 240 bytes; engine::renderer::ClusterParams).
public struct ClusterParams {
    public float4 view[4];     // world -> view (columns)
    public float4 proj[4];     // view -> clip (columns)
    public float4 inv_proj[4]; // clip -> view (columns)
    public uint4  dims;        // grid x, y, z; w = light count
    public float4 depth;       // near, far, slices / ln(far/near), ln(near) * z
    public uint4  limits;      // x = index-list capacity
}

public float4 cluster_mul(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

public uint cluster_linear_index(ClusterParams p, uint3 c) {
    return (c.z * p.dims.y + c.y) * p.dims.x + c.x;
}

// Depth slice of a positive view-space distance, clamped to the grid.
public uint cluster_depth_slice(ClusterParams p, float view_depth) {
    if (!(view_depth > 0.0)) {
        return 0;
    }
    float s = floor(log(view_depth) * p.depth.z - p.depth.w);
    return uint(clamp(s, 0.0, float(p.dims.z - 1)));
}

// View-space distance of slice boundary k (0 = near, dims.z = far).
public float cluster_slice_boundary(ClusterParams p, uint k) {
    return p.depth.x * pow(p.depth.y / p.depth.x, float(k) / float(p.dims.z));
}

// Cluster containing a view-space position (camera looks down -Z).
public uint3 cluster_of_view_position(ClusterParams p, float3 view_pos) {
    float4 clip = cluster_mul(p.proj, float4(view_pos, 1.0));
    float2 uv = clip.xy / max(clip.w, 1e-6) * 0.5 + 0.5;
    float2 tile = clamp(floor(uv * float2(p.dims.xy)), float2(0.0, 0.0),
                        float2(p.dims.xy) - 1.0);
    return uint3(uint(tile.x), uint(tile.y), cluster_depth_slice(p, -view_pos.z));
}

// Linear cluster index of a world-space sample point. Shading passes map the
// point through the same view/projection the cull pass built the clusters
// from, so the lookup is exact regardless of TAA jitter in the raster matrices.
public uint cluster_index_of_world(ClusterParams p, float3 world_pos) {
    float3 v = cluster_mul(p.view, float4(world_pos, 1.0)).xyz;
    return cluster_linear_index(p, cluster_of_view_position(p, v));
}

// Conservative view-space AABB of cluster c: the box around the points where
// the tile's four corner rays cross the slice's near and far depths.
public void cluster_aabb(ClusterParams p, uint3 c, out float3 box_min, out float3 box_max) {
    float2 dims = float2(p.dims.xy);
    float2 ndc_min = float2(c.xy) / dims * 2.0 - 1.0;
    float2 ndc_max = float2(c.xy + 1) / dims * 2.0 - 1.0;
    float z_near = cluster_slice_boundary(p, c.z);
    float z_far = cluster_slice_boundary(p, c.z + 1);
    box_min = float3(1e30, 1e30, 1e30);
    box_max = float3(-1e30, -1e30, -1e30);
    [unroll]
    for (uint i = 0; i < 4; ++i) {
        float2 ndc = float2((i & 1) != 0 ? ndc_max.x : ndc_min.x,
                            (i & 2) != 0 ? ndc_max.y : ndc_min.y);
        // Any NDC depth inside (0, 1) is on the ray at a finite distance.
        float4 h = cluster_mul(p.inv_proj, float4(ndc, 0.5, 1.0));
        float3 v = h.xyz / h.w;
        float3 ray = v / -v.z; // scaled to view z = -1
        box_min = min(box_min, min(ray * z_near, ray * z_far));
        box_max = max(box_max, max(ray * z_near, ray * z_far));
    }
}

// Whether a view-space sphere overlaps the box.
public bool cluster_sphere_overlaps(float3 box_min, float3 box_max, float3 centre, float radius) {
    float3 d = max(max(box_min - centre, centre - box_max), float3(0.0, 0.0, 0.0));
    return radius > 0.0 && dot(d, d) <= radius * radius;
}

// Smooth range-limited falloff: windowed inverse-square.
public float light_range_attenuation(float dist, float range) {
    float t = saturate(1.0 - pow(dist / range, 4.0));
    return t * t / (dist * dist + 1.0);
}

// Radiance `light` delivers at `world_pos` and the unit direction towards it.
// Returns false (no contribution) beyond the light's range.
public bool light_incident(GpuLight light, float3 world_pos, out float3 to_light,
                           out float3 radiance) {
    float3 delta = light.position_range.xyz - world_pos;
    float dist = length(delta);
    to_light = delta / max(dist, 1e-4);
    radiance = float3(0.0, 0.0, 0.0);
    if (dist >= light.position_range.w) {
        return false;
    }
    float att = light_range_attenuation(dist, light.position_range.w);
    if (light.type == LIGHT_TYPE_SPOT) {
        float cd = dot(normalize(light.direction), -to_light);
        float a = saturate(cd * light.spot_scale_offset.x + light.spot_scale_offset.y);
        att *= a * a;
    }
    radiance = light.color_intensity.rgb * light.color_intensity.w * att;
    return true;
}
