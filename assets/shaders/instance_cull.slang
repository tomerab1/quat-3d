// instance_cull.slang — per-view GPU instance culling (ROADMAP R6 / R9.4).
//
// The data contract and the CPU reference these kernels mirror operation for
// operation are in include/engine/renderer/instance_cull.hpp.
//
// compute_main: one workgroup per chunk of up to 64 instances of one draw
// (the CPU lists the chunks, so no per-instance records are uploaded). Each
// thread builds its instance's world bounding sphere once and tests it
// against every view: the camera's planes, or a sun-shadow cascade whose
// planes come from the GPU-fitted cascade buffer (Gribb & Hartmann 2001,
// "Fast Extraction of Viewing Frustum Planes from the World-View-Projection
// Matrix"), its near plane pushed toward the sun by the caster reach, plus
// the projected-size and caster-distance tests for shadow views. A survivor
// takes a slot in its draw's range of the view's visible list with one
// atomic on that (view, draw)'s counter. The counters are descriptor-bound
// storage buffers: atomics through buffer-device-address pointers are
// mistranslated by MoltenVK's SPIRV-Cross (issue #130).
//
// finalize_main: one thread per (view, draw) writes the draw's indirect
// commands (one per submesh: its index range, the survivors as the instance
// count, the draw's range start as firstInstance) and its draw count for
// vkCmdDrawIndexedIndirectCount (the submesh count, or 0 when nothing
// survived). The structure follows Haar & Aaltonen 2015, "GPU-Driven
// Rendering Pipelines" (SIGGRAPH Advances in Real-Time Rendering).

import csm;
import instancing;

static const uint CHUNK = 64;
static const uint MAX_VIEWS = 5;
static const uint KIND_PLANES = 0;
static const uint KIND_CASCADE = 1;
static const uint NEVER_CULL = 1;
// Receiver-slice filter margin in shadow texels (instance_cull.hpp
// cull_receiver_margin_texels).
static const float RECEIVER_MARGIN_TEXELS = 32.0;

// Mirrors engine::renderer::CullDrawGpu (48 bytes).
struct CullDraw {
    float4 local_sphere;  // object-space bounds: xyz centre, w radius
    uint   first_instance;
    uint   instance_count;
    uint   first_command;
    uint   command_count;
    float  shadow_distance; // shadow views: max caster distance, 0 = unlimited
    uint   flags;
    uint   pad0;
    uint   pad1;
}

// Mirrors engine::renderer::CullViewGpu (112 bytes).
struct CullView {
    float4 planes[6]; // kind planes: inward, normalised
    uint   kind;
    uint   cascade;
    float  min_caster_texels;
    uint   pad;
}

// Mirrors engine::renderer::CullParamsGpu (688 bytes).
struct CullParams {
    float4   camera;
    uint     view_count;
    uint     chunk_count;
    uint     draw_count;
    uint     command_count;  // per view
    uint     instance_count; // per view (visible list stride)
    uint     groups_x;
    uint     has_receiver;   // 1: cascade views test the receiver slices
    uint     pad1;
    float4   receiver_sides[4]; // receiver camera side planes, inward
    float4   receiver_depth;    // view depth as a plane: dot(xyz, x) + w
    CullView views[MAX_VIEWS];
}

ConstantBuffer<CullParams>       g_params;
StructuredBuffer<InstanceData>   g_instances;
StructuredBuffer<CullDraw>       g_draws;
StructuredBuffer<uint2>          g_chunks;    // (draw, first instance of the chunk within it)
StructuredBuffer<CascadeData>    g_cascades;
StructuredBuffer<uint2>          g_submeshes; // per command: (index count, first index)
RWStructuredBuffer<uint>         g_counts;    // [view * draw_count + draw]
RWStructuredBuffer<uint>         g_visible;   // [view * instance_count + slot]
RWStructuredBuffer<uint>         g_commands;  // VkDrawIndexedIndirectCommand (5 uints) each
RWStructuredBuffer<uint>         g_draw_counts; // [view * draw_count + draw]

// Inward, normalised plane from a row of the clip matrix (cascade c).
float4 normalised(float4 p) {
    float len = length(p.xyz);
    return len > 0.0 ? p / len : p;
}

// Row r of cascade c's view-projection (stored as 4 columns).
float4 cascade_row(CascadeData d, uint c, uint r) {
    return float4(d.view_proj[c * 4][r], d.view_proj[c * 4 + 1][r], d.view_proj[c * 4 + 2][r],
                  d.view_proj[c * 4 + 3][r]);
}

// The cull test for one view (instance_cull.hpp cull_accepts).
bool accepts(CullView view, CullDraw draw, float3 centre, float radius) {
    if (view.kind != KIND_PLANES && view.kind != KIND_CASCADE) return true;
    if ((draw.flags & NEVER_CULL) != 0) return true;
    float4 planes[6];
    float near_extrude = 0.0;
    if (view.kind == KIND_CASCADE) {
        CascadeData d = g_cascades[0];
        uint c = min(view.cascade, 3u);
        float4 r0 = cascade_row(d, c, 0);
        float4 r1 = cascade_row(d, c, 1);
        float4 r2 = cascade_row(d, c, 2);
        float4 r3 = cascade_row(d, c, 3);
        planes[0] = normalised(r3 + r0);
        planes[1] = normalised(r3 - r0);
        planes[2] = normalised(r3 + r1);
        planes[3] = normalised(r3 - r1);
        planes[4] = normalised(r2);
        planes[5] = normalised(r3 - r2);
        near_extrude = max(d.limits.y, 0.0);
        if (g_params.has_receiver != 0) {
            // The caster's sphere swept away from the sun across the
            // cascade must reach the camera slice this cascade shades.
            float prev_start = c >= 2 ? d.split_far[c - 2] : d.params.y;
            float start = c == 0 ? d.params.y : d.split_far[c - 1];
            if (c > 0) start -= d.params.z * (start - prev_start);
            float3 b = centre + planes[4].xyz * (d.depth_range[c] + near_extrude);
            float slack = radius + RECEIVER_MARGIN_TEXELS * d.texel_world[c];
            for (uint i = 0; i < 4; ++i) {
                float4 p = g_params.receiver_sides[i];
                float da = dot(p.xyz, centre) + p.w;
                float db = dot(p.xyz, b) + p.w;
                if (max(da, db) < -slack) return false;
            }
            float4 f = g_params.receiver_depth;
            float za = dot(f.xyz, centre) + f.w;
            float zb = dot(f.xyz, b) + f.w;
            if (max(za, zb) < start - slack) return false;
            if (min(za, zb) > d.split_far[c] + slack) return false;
        }
    } else {
        for (uint i = 0; i < 6; ++i) planes[i] = view.planes[i];
    }
    for (uint i = 0; i < 6; ++i) {
        float slack = i == 4 ? near_extrude : 0.0;
        if (dot(planes[i].xyz, centre) + planes[i].w < -(radius + slack)) return false;
    }
    if (view.kind == KIND_CASCADE) {
        CascadeData d = g_cascades[0];
        float texel = d.texel_world[min(view.cascade, 3u)];
        if (texel > 0.0 && view.min_caster_texels > 0.0 &&
            2.0 * radius < view.min_caster_texels * texel) {
            return false;
        }
        if (draw.shadow_distance > 0.0 &&
            length(centre - g_params.camera.xyz) - radius > draw.shadow_distance) {
            return false;
        }
    }
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    uint chunk = group.y * g_params.groups_x + group.x;
    if (chunk >= g_params.chunk_count) return;
    uint2 work = g_chunks[chunk];
    CullDraw draw = g_draws[work.x];
    uint local = work.y + thread.x;
    if (local >= draw.instance_count) return;
    uint index = draw.first_instance + local;

    // World bounding sphere: centre through the model, radius scaled by the
    // largest axis scale (instance_cull.hpp world_sphere).
    InstanceData inst = g_instances[index];
    float3 centre = inst.world_position(draw.local_sphere.xyz).xyz;
    float3x3 m = (float3x3)inst.model;
    float sx = dot(mul(m, float3(1, 0, 0)), mul(m, float3(1, 0, 0)));
    float sy = dot(mul(m, float3(0, 1, 0)), mul(m, float3(0, 1, 0)));
    float sz = dot(mul(m, float3(0, 0, 1)), mul(m, float3(0, 0, 1)));
    float radius = draw.local_sphere.w * sqrt(max(sx, max(sy, sz)));

    for (uint v = 0; v < g_params.view_count; ++v) {
        if (!accepts(g_params.views[v], draw, centre, radius)) continue;
        uint slot;
        InterlockedAdd(g_counts[v * g_params.draw_count + work.x], 1u, slot);
        g_visible[v * g_params.instance_count + draw.first_instance + slot] = index;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void finalize_main(uint3 id : SV_DispatchThreadID) {
    uint total = g_params.view_count * g_params.draw_count;
    if (id.x >= total) return;
    uint v = id.x / g_params.draw_count;
    uint d = id.x - v * g_params.draw_count;
    CullDraw draw = g_draws[d];
    uint count = g_counts[id.x];
    for (uint k = 0; k < draw.command_count; ++k) {
        uint cmd = (v * g_params.command_count + draw.first_command + k) * 5;
        uint2 sub = g_submeshes[draw.first_command + k];
        g_commands[cmd + 0] = sub.x;               // indexCount
        g_commands[cmd + 1] = count;               // instanceCount
        g_commands[cmd + 2] = sub.y;               // firstIndex
        g_commands[cmd + 3] = 0;                   // vertexOffset
        g_commands[cmd + 4] = draw.first_instance; // firstInstance
    }
    g_draw_counts[id.x] = count > 0 ? draw.command_count : 0;
}
