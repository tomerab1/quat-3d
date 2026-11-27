// particle_sort.slang — back-to-front bitonic sort of an emitter's drawn
// alive list (ROADMAP R10.3), for emitters that ask for a stable draw order.
//
// Keys are (far-first depth key, pool slot) pairs over a power-of-two range
// of the emitter's capacity; slots past the alive count get the maximum key
// and sort to the end. The slot breaks depth ties, so the order is the same
// whatever order the simulation appended the survivors in. Entry points
// (one dispatch each, the pass inserts compute barriers between them):
//   keys_main    — one thread per key: builds the key array;
//   local_main   — sorts each 512-key block in shared memory (stages k <= 512);
//   global_main  — one compare-exchange step (k, j >= 512) in device memory;
//   merge_main   — finishes stage k for j < 512 in shared memory;
//   scatter_main — writes the sorted slots back into the alive list.

import particle_common;

ConstantBuffer<ParticleFrame> g_frame;
StructuredBuffer<Emitter>     g_emitters;
StructuredBuffer<Particle>    g_particles;
RWStructuredBuffer<uint>      g_alive;
StructuredBuffer<uint>        g_counters;
RWStructuredBuffer<uint2>     g_keys;

// Mirrors engine::renderer ParticlePush (particle_pass.cpp).
struct PushConstants {
    uint emitter;
    uint k; // bitonic stage size
    uint j; // compare distance
    uint pad;
}
[[vk::push_constant]] PushConstants pc;

static const uint BLOCK = 512; // keys per shared-memory block (two per thread)
static const uint MAX_KEY = 0xFFFFFFFFu;

groupshared uint2 s_keys[BLOCK];

bool key_less(uint2 a, uint2 b) {
    return a.x < b.x || (a.x == b.x && a.y < b.y);
}

// Orders the pair (i, i + j) ascending when (i & k) == 0, else descending.
void exchange_shared(uint i, uint j, uint k, uint global_i) {
    uint2 a = s_keys[i];
    uint2 b = s_keys[i + j];
    bool ascending = (global_i & k) == 0;
    if (key_less(b, a) == ascending) {
        s_keys[i] = b;
        s_keys[i + j] = a;
    }
}

uint drawn_count(Emitter e) {
    return g_counters[e.ids.z * COUNTER_STRIDE + COUNTER_ALIVE0 + g_frame.misc.z];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void keys_main(uint3 tid : SV_DispatchThreadID) {
    Emitter e = g_emitters[pc.emitter];
    uint i = tid.x;
    if (i >= e.ids.y) {
        return;
    }
    uint2 key = uint2(MAX_KEY, MAX_KEY);
    if (i < drawn_count(e)) {
        uint slot = g_alive[g_frame.misc.z * g_frame.misc.y + e.range.x + i];
        float3 wp = particle_world_position(e, g_particles[slot].position);
        float depth = max(particle_view_depth(g_frame, wp), 0.0);
        key = uint2(0xFFFFFFFEu - asuint(depth), slot); // farther = smaller key
    }
    g_keys[e.ids.x + i] = key;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void local_main(uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    Emitter e = g_emitters[pc.emitter];
    uint block = group.x * BLOCK;
    s_keys[local] = g_keys[e.ids.x + block + local];
    s_keys[local + 256] = g_keys[e.ids.x + block + local + 256];
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2; k <= BLOCK; k <<= 1) {
        for (uint j = k >> 1; j > 0; j >>= 1) {
            uint i = 2 * j * (local / j) + (local % j);
            exchange_shared(i, j, k, block + i);
            GroupMemoryBarrierWithGroupSync();
        }
    }
    g_keys[e.ids.x + block + local] = s_keys[local];
    g_keys[e.ids.x + block + local + 256] = s_keys[local + 256];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void global_main(uint3 tid : SV_DispatchThreadID) {
    Emitter e = g_emitters[pc.emitter];
    uint t = tid.x;
    if (t >= e.ids.y / 2) {
        return;
    }
    uint j = pc.j;
    uint i = 2 * j * (t / j) + (t % j);
    uint2 a = g_keys[e.ids.x + i];
    uint2 b = g_keys[e.ids.x + i + j];
    bool ascending = (i & pc.k) == 0;
    if (key_less(b, a) == ascending) {
        g_keys[e.ids.x + i] = b;
        g_keys[e.ids.x + i + j] = a;
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void merge_main(uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    Emitter e = g_emitters[pc.emitter];
    uint block = group.x * BLOCK;
    s_keys[local] = g_keys[e.ids.x + block + local];
    s_keys[local + 256] = g_keys[e.ids.x + block + local + 256];
    GroupMemoryBarrierWithGroupSync();
    for (uint j = BLOCK >> 1; j > 0; j >>= 1) {
        uint i = 2 * j * (local / j) + (local % j);
        exchange_shared(i, j, pc.k, block + i);
        GroupMemoryBarrierWithGroupSync();
    }
    g_keys[e.ids.x + block + local] = s_keys[local];
    g_keys[e.ids.x + block + local + 256] = s_keys[local + 256];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void scatter_main(uint3 tid : SV_DispatchThreadID) {
    Emitter e = g_emitters[pc.emitter];
    uint i = tid.x;
    if (i >= min(drawn_count(e), e.ids.y)) {
        return;
    }
    g_alive[g_frame.misc.z * g_frame.misc.y + e.range.x + i] = g_keys[e.ids.x + i].y;
}
