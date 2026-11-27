// terrain_shadow.slang — terrain chunks into a sun shadow cascade (R12).
//
// The depth-only twin of terrain.slang's vertex stage: the same vertex-id
// grid (lib/terrain_grid.slang) and heightmap fetch, so the caster geometry
// matches what the GBuffer drew at the same LOD (no self-shadowing from a
// mismatched surface), projected with the cascade's matrix from the
// CascadeData buffer (GPU-fitted; read through a buffer device address).

import csm;
import terrain_grid;

// Mirrors TerrainShadowPush in terrain_shadow.cpp (64 bytes).
struct PushConstants {
    CascadeData* cascades;
    uint         cascade;
    uint         pad;
    float4       chunk;  // x,y = chunk origin (world xz), z = chunk size m, w = skirt depth m
    float4       region; // x,y = heightmap uv offset, z = uv scale, w unused
    float4       grid;   // x = grid_n, y = tile origin world y
}
[[vk::push_constant]] PushConstants pc;

Texture2D    g_heightmap; // R32F, metres
SamplerState g_sampler;   // linear clamp

[shader("vertex")]
float4 vertex_main(uint id : SV_VertexID) : SV_Position {
    float2 g;
    bool skirt;
    terrain_decode_vertex(id, uint(pc.grid.x), g, skirt);
    const float2 local01 = g / pc.grid.x;
    const float2 uv = pc.region.xy + local01 * pc.region.z;
    float h = pc.grid.y + g_heightmap.SampleLevel(g_sampler, uv, 0.0).r;
    if (skirt) {
        h -= pc.chunk.w;
    }
    float4 world = float4(pc.chunk.x + local01.x * pc.chunk.z, h,
                          pc.chunk.y + local01.y * pc.chunk.z, 1.0);
    uint b = pc.cascade * 4;
    return pc.cascades[0].view_proj[b] * world.x + pc.cascades[0].view_proj[b + 1] * world.y +
           pc.cascades[0].view_proj[b + 2] * world.z + pc.cascades[0].view_proj[b + 3];
}

[shader("fragment")]
void fragment_main() {}
