// particle_sim.slang — GPU particle emission and simulation (ROADMAP R10.3).
//
// The pool is one particle array shared by every emitter; each emitter owns a
// contiguous range [base, base + capacity) of it, and of the dead list and
// the two alive lists (alive list k starts at k * pool capacity). Each
// emitter's counters name its live list, "cur". Entry points, in frame order
// (the pass records one dispatch per emitter unless noted):
//   reset_main    — fills a new emitter's dead list with its whole range and
//                   zeroes its counters and indirect arguments;
//   emit_main     — thread i < min(requested, dead) pops dead[dead - 1 - i]
//                   (no atomics: emission is deterministic), spawns particle
//                   (first spawn index + i) into it and appends it to cur;
//   commit_main   — one thread per emitter: applies the pops, zeroes the
//                   other list's count and writes the simulate dispatch size;
//   simulate_main — indirect, one thread per particle of cur: ages,
//                   integrates, collides with the depth buffer, and appends
//                   survivors to the other list / the dead to the dead list;
//   finalize_main — one thread per emitter: writes the draw-indexed
//                   arguments (6 indices, one instance per survivor), zeroes
//                   cur's count and makes the survivors' list the live one.
// The list index lives only on the GPU: nothing on the host has to agree
// with which recorded frames actually executed.

import particle_common;

ConstantBuffer<ParticleFrame> g_frame;
StructuredBuffer<Emitter>     g_emitters;
RWStructuredBuffer<Particle>  g_particles;
RWStructuredBuffer<uint>      g_dead;
RWStructuredBuffer<uint>      g_alive;
RWStructuredBuffer<uint>      g_counters; // COUNTER_STRIDE per emitter
RWStructuredBuffer<uint>      g_dispatch; // DISPATCH_STRIDE per emitter
RWStructuredBuffer<uint>      g_draw;     // DRAW_STRIDE per emitter
Texture2D<float>              g_depth;    // GBuffer depth
Texture2D<float2>             g_normal;   // GBuffer normal (octahedral)

// Mirrors engine::renderer ParticlePush (particle_pass.cpp).
struct PushConstants {
    uint emitter; // emitter index, or the emitter count for per-emitter passes
    uint k;       // unused here (sort stage)
    uint j;
    uint pad;
}
[[vk::push_constant]] PushConstants pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void reset_main(uint3 tid : SV_DispatchThreadID) {
    Emitter e = g_emitters[pc.emitter];
    uint base = e.range.x;
    uint capacity = e.range.y;
    if (tid.x < capacity) {
        g_dead[base + tid.x] = base + capacity - 1 - tid.x; // pops take base, base + 1, ...
    }
    if (tid.x == 0) {
        uint c = e.ids.z * COUNTER_STRIDE;
        g_counters[c + COUNTER_DEAD] = capacity;
        g_counters[c + COUNTER_ALIVE0] = 0;
        g_counters[c + COUNTER_ALIVE0 + 1] = 0;
        g_counters[c + COUNTER_LIST] = 0;
        uint d = e.ids.z * DRAW_STRIDE;
        g_draw[d + 0] = 6;
        g_draw[d + 1] = 0;
        g_draw[d + 2] = 0;
        g_draw[d + 3] = 0;
        g_draw[d + 4] = 0;
        uint g = e.ids.z * DISPATCH_STRIDE;
        g_dispatch[g + 0] = 0;
        g_dispatch[g + 1] = 1;
        g_dispatch[g + 2] = 1;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void emit_main(uint3 tid : SV_DispatchThreadID) {
    Emitter e = g_emitters[pc.emitter];
    uint c = e.ids.z * COUNTER_STRIDE;
    uint dead = g_counters[c + COUNTER_DEAD];
    uint count = min(e.range.z, dead);
    uint i = tid.x;
    if (i >= count) {
        return;
    }
    uint cur = g_counters[c + COUNTER_LIST];
    uint alive = g_counters[c + COUNTER_ALIVE0 + cur];
    uint base = e.range.x;
    uint slot = g_dead[base + dead - 1 - i];
    g_particles[slot] = particle_spawn(e, e.range.w + i);
    g_alive[cur * g_frame.misc.y + base + alive + i] = slot;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void commit_main(uint3 tid : SV_DispatchThreadID) {
    uint index = tid.x;
    if (index >= pc.emitter) {
        return;
    }
    Emitter e = g_emitters[index];
    uint c = e.ids.z * COUNTER_STRIDE;
    uint cur = g_counters[c + COUNTER_LIST];
    uint dead = g_counters[c + COUNTER_DEAD];
    uint count = min(e.range.z, dead);
    uint alive = g_counters[c + COUNTER_ALIVE0 + cur] + count;
    g_counters[c + COUNTER_DEAD] = dead - count;
    g_counters[c + COUNTER_ALIVE0 + cur] = alive;
    g_counters[c + COUNTER_ALIVE0 + (1 - cur)] = 0;
    uint g = e.ids.z * DISPATCH_STRIDE;
    g_dispatch[g + 0] = (alive + 63) / 64;
    g_dispatch[g + 1] = 1;
    g_dispatch[g + 2] = 1;
}

// Depth-buffer collision (AMD "Holy Smoke"): a particle that went behind the
// visible surface, by less than the collision thickness, is pushed back onto
// it and its velocity reflected about the GBuffer normal (restitution
// `bounce`, tangential speed scaled by 1 - friction). Off-screen or occluded
// geometry is invisible to it — the technique's known limit.
void collide(inout Particle p, Emitter e) {
    bool local = (e.config.y & PARTICLE_FLAG_LOCAL) != 0;
    float3 wp = local ? particle_mul(e.world, float4(p.position, 1.0)).xyz : p.position;
    float4 clip = particle_mul(g_frame.view_proj, float4(wp, 1.0));
    if (clip.w <= 1e-4) {
        return;
    }
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) >= 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) {
        return;
    }
    uint2 size = uint2(g_frame.screen.xy);
    uint2 px = min(uint2((ndc.xy * 0.5 + 0.5) * g_frame.screen.xy), size - 1);
    float d = g_depth.Load(int3(px, 0));
    if (d >= 1.0) {
        return; // sky
    }
    float surface_depth = particle_linear_depth(g_frame, ndc.xy, d);
    float particle_depth = particle_view_depth(g_frame, wp);
    if (particle_depth < surface_depth || particle_depth > surface_depth + e.render.z) {
        return;
    }
    float3 n = particle_oct_decode(g_normal.Load(int3(px, 0)));
    float4 sw = particle_mul(g_frame.inv_view_proj, float4(ndc.xy, d, 1.0));
    float3 surface = sw.xyz / sw.w;

    float3 wv = local ? particle_mul(e.world, float4(p.velocity, 0.0)).xyz : p.velocity;
    float vn = dot(wv, n);
    if (vn < 0.0) {
        float3 vt = wv - vn * n;
        wv = vt * e.spin_hit.w - n * (vn * e.spin_hit.z);
    }
    float dist = dot(wp - surface, n);
    if (dist < 0.0) {
        wp -= n * (dist - 1e-3);
    }
    if (local) {
        p.position = particle_mul(e.inv_world, float4(wp, 1.0)).xyz;
        p.velocity = particle_mul(e.inv_world, float4(wv, 0.0)).xyz;
    } else {
        p.position = wp;
        p.velocity = wv;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void simulate_main(uint3 tid : SV_DispatchThreadID) {
    Emitter e = g_emitters[pc.emitter];
    uint c = e.ids.z * COUNTER_STRIDE;
    uint cur = g_counters[c + COUNTER_LIST];
    uint i = tid.x;
    if (i >= g_counters[c + COUNTER_ALIVE0 + cur]) {
        return;
    }
    uint pool = g_frame.misc.y;
    uint base = e.range.x;
    uint slot = g_alive[cur * pool + base + i];
    Particle p = g_particles[slot];
    float dt = g_frame.camera.w;

    p.age += dt;
    if (p.age >= p.lifetime) {
        uint d;
        InterlockedAdd(g_counters[c + COUNTER_DEAD], 1, d);
        g_dead[base + d] = slot;
        return;
    }
    p.velocity = (p.velocity + e.forces.xyz * dt) * e.forces.w;
    p.position += p.velocity * dt;
    p.rotation += p.spin * dt;
    if ((e.config.y & PARTICLE_FLAG_COLLIDE) != 0) {
        collide(p, e);
    }
    g_particles[slot] = p;
    uint next = 1 - cur;
    uint a;
    InterlockedAdd(g_counters[c + COUNTER_ALIVE0 + next], 1, a);
    g_alive[next * pool + base + a] = slot;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void finalize_main(uint3 tid : SV_DispatchThreadID) {
    uint index = tid.x;
    if (index >= pc.emitter) {
        return;
    }
    uint slot = g_emitters[index].ids.z;
    uint c = slot * COUNTER_STRIDE;
    uint cur = g_counters[c + COUNTER_LIST];
    uint d = slot * DRAW_STRIDE;
    g_draw[d + 0] = 6;
    g_draw[d + 1] = g_counters[c + COUNTER_ALIVE0 + (1 - cur)];
    g_draw[d + 2] = 0;
    g_draw[d + 3] = 0;
    g_draw[d + 4] = 0;
    g_counters[c + COUNTER_ALIVE0 + cur] = 0;
    g_counters[c + COUNTER_LIST] = 1 - cur;
}
