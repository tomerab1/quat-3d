// particle_common.slang — GPU particle records, hashing, spawn and curves
// (ROADMAP R10.3). Layouts and the spawn / integration math mirror
// engine/renderer/particles.hpp, whose CPU references the tests compare
// against (keep the two in step). Matrices are stored as four column vectors
// (glm column-major) and applied with particle_mul.

public static const uint PARTICLE_FLAG_LOCAL    = 1u << 0;
public static const uint PARTICLE_FLAG_COLLIDE  = 1u << 1;
public static const uint PARTICLE_FLAG_LIT      = 1u << 2;
public static const uint PARTICLE_FLAG_ADDITIVE = 1u << 3;
public static const uint PARTICLE_FLAG_RESET    = 1u << 4;
public static const uint PARTICLE_FLAG_SORT     = 1u << 5;

public static const uint SHAPE_POINT  = 0;
public static const uint SHAPE_SPHERE = 1;
public static const uint SHAPE_BOX    = 2;
public static const uint SHAPE_CONE   = 3;

// Per-emitter counters (uint4 each, in a uint buffer): dead count, alive
// count of list 0 / list 1, particles emitted this frame.
public static const uint COUNTER_DEAD = 0;
public static const uint COUNTER_ALIVE0 = 1;
public static const uint COUNTER_EMITTED = 3;
public static const uint COUNTER_STRIDE = 4;
// Indirect arguments per emitter: dispatch (uint4), draw-indexed (8 uints).
public static const uint DISPATCH_STRIDE = 4;
public static const uint DRAW_STRIDE = 8;

public static const float TWO_PI = 6.28318530718;

// engine::renderer::ParticleGpu (std430, 48 bytes).
public struct Particle {
    public float3 position;
    public float  age;
    public float3 velocity;
    public float  lifetime;
    public float  rotation;
    public float  spin;
    public float  size;
    public uint   random;
}

// engine::renderer::EmitterGpu (std430, 416 bytes).
public struct Emitter {
    public float4 world[4];
    public float4 inv_world[4];
    public uint4  range;      // pool base, capacity, requested, first spawn index
    public uint4  config;     // shape, flags, seed, color keys | size keys << 8
    public float4 shape;      // box half extents, radius
    public float4 direction;  // unit axis, 1 - cos(spread or cone)
    public float4 speed_life; // speed min, max; lifetime min, max
    public float4 forces;     // gravity (simulation space), 1 / (1 + drag dt)
    public float4 size_rot;   // size min, max; rotation min, max
    public float4 spin_hit;   // spin min, max; bounce; 1 - friction
    public float4 flipbook;   // columns, rows, frames, fps
    public float4 render;     // 1 / soft distance, emissive, collision thickness
    public uint4  ids;        // sort key base, sort key count, emitter slot
    public float4 color_value[4];
    public float4 color_time;
    public float4 size_value;
    public float4 size_time;
}

// engine::renderer::ParticleFrameGpu (std140, 384 bytes).
public struct ParticleFrame {
    public float4 view_proj[4];     // world -> clip (the frame's jittered raster matrix)
    public float4 inv_view_proj[4]; // clip -> world
    public float4 view[4];          // world -> view
    public float4 inv_proj[4];      // clip -> view (depth linearisation)
    public float4 camera;           // xyz position, w = dt
    public float4 camera_right;     // world-space billboard axes
    public float4 camera_up;
    public float4 sun_direction;    // xyz towards the sun
    public float4 sun_color;        // rgb x intensity
    public float4 ambient;          // rgb
    public float4 screen;           // width, height, 1 / width, 1 / height
    public uint4  misc;             // x = list simulated, y = pool capacity, z = list drawn,
                                    // w = emitter count
}

public float4 particle_mul(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

public uint particle_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

public float particle_random01(uint seed, uint index, uint stream) {
    uint h = particle_hash(particle_hash(index ^ particle_hash(seed)) + stream);
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// Orthonormal basis around unit n (Duff et al., JCGT 2017).
public void particle_basis(float3 n, out float3 t, out float3 b) {
    float sgn = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sgn + n.z);
    float k = n.x * n.y * a;
    t = float3(1.0 + sgn * n.x * n.x * a, sgn * k, -sgn * n.x);
    b = float3(k, sgn + n.y * n.y * a, -n.y);
}

// The particle `index` of emitter `e` as born (simulation space).
public Particle particle_spawn(Emitter e, uint index) {
    uint seed = e.config.z;
    float3 axis = e.direction.xyz;
    float3 t;
    float3 b;
    particle_basis(axis, t, b);

    float3 pos = float3(0.0, 0.0, 0.0);
    float3 dir = axis;
    float u = particle_random01(seed, index, 3);
    float v = particle_random01(seed, index, 4);
    float c = 1.0 - u * e.direction.w;
    float sn = sqrt(max(1.0 - c * c, 0.0));
    float3 cone_dir = t * (sn * cos(TWO_PI * v)) + b * (sn * sin(TWO_PI * v)) + axis * c;
    uint shape = e.config.x;
    if (shape == SHAPE_SPHERE) {
        float z = 1.0 - 2.0 * particle_random01(seed, index, 0);
        float rr = sqrt(max(1.0 - z * z, 0.0));
        float phi = TWO_PI * particle_random01(seed, index, 1);
        dir = float3(rr * cos(phi), rr * sin(phi), z);
        pos = dir * (e.shape.w * pow(particle_random01(seed, index, 2), 1.0 / 3.0));
    } else if (shape == SHAPE_BOX) {
        float3 q = float3(particle_random01(seed, index, 0), particle_random01(seed, index, 1),
                          particle_random01(seed, index, 2));
        pos = (q * 2.0 - 1.0) * e.shape.xyz;
        dir = cone_dir;
    } else if (shape == SHAPE_CONE) {
        float rr = e.shape.w * sqrt(particle_random01(seed, index, 0));
        float phi = TWO_PI * particle_random01(seed, index, 1);
        pos = t * (rr * cos(phi)) + b * (rr * sin(phi));
        dir = cone_dir;
    } else {
        dir = cone_dir;
    }
    float speed = lerp(e.speed_life.x, e.speed_life.y, particle_random01(seed, index, 5));

    Particle p;
    if ((e.config.y & PARTICLE_FLAG_LOCAL) != 0) {
        p.position = pos;
        p.velocity = dir * speed;
    } else {
        p.position = particle_mul(e.world, float4(pos, 1.0)).xyz;
        float3 wd = particle_mul(e.world, float4(dir, 0.0)).xyz;
        float len = length(wd);
        p.velocity = len > 1e-12 ? wd * (speed / len) : float3(0.0, 0.0, 0.0);
    }
    p.age = 0.0;
    p.lifetime = lerp(e.speed_life.z, e.speed_life.w, particle_random01(seed, index, 6));
    p.size = lerp(e.size_rot.x, e.size_rot.y, particle_random01(seed, index, 7));
    p.rotation = lerp(e.size_rot.z, e.size_rot.w, particle_random01(seed, index, 8));
    p.spin = lerp(e.spin_hit.x, e.spin_hit.y, particle_random01(seed, index, 9));
    p.random = particle_hash(index ^ particle_hash(seed + 0x9E3779B9u));
    return p;
}

// Piecewise-linear curves over the normalised age (clamped at the ends).
public float4 particle_color_at(Emitter e, float t) {
    uint n = e.config.w & 0xFFu;
    if (n == 0 || t <= e.color_time[0]) {
        return e.color_value[0];
    }
    for (uint k = 1; k < n; ++k) {
        float t1 = e.color_time[k];
        if (t <= t1) {
            float t0 = e.color_time[k - 1];
            return lerp(e.color_value[k - 1], e.color_value[k], (t - t0) / max(t1 - t0, 1e-6));
        }
    }
    return e.color_value[n - 1];
}

public float particle_size_at(Emitter e, float t) {
    uint n = (e.config.w >> 8u) & 0xFFu;
    if (n == 0 || t <= e.size_time[0]) {
        return e.size_value[0];
    }
    for (uint k = 1; k < n; ++k) {
        float t1 = e.size_time[k];
        if (t <= t1) {
            float t0 = e.size_time[k - 1];
            return lerp(e.size_value[k - 1], e.size_value[k], (t - t0) / max(t1 - t0, 1e-6));
        }
    }
    return e.size_value[n - 1];
}

// Simulation-space -> world position of a particle of `e`.
public float3 particle_world_position(Emitter e, float3 p) {
    if ((e.config.y & PARTICLE_FLAG_LOCAL) != 0) {
        return particle_mul(e.world, float4(p, 1.0)).xyz;
    }
    return p;
}

// Positive view-space distance of a world position.
public float particle_view_depth(ParticleFrame f, float3 world_pos) {
    return -particle_mul(f.view, float4(world_pos, 1.0)).z;
}

// Positive view-space distance of a depth-buffer sample at NDC xy.
public float particle_linear_depth(ParticleFrame f, float2 ndc, float depth) {
    float4 v = particle_mul(f.inv_proj, float4(ndc, depth, 1.0));
    return -v.z / v.w;
}

// Octahedral-decoded GBuffer normal (inverse of gbuffer.slang's oct_encode).
public float3 particle_oct_decode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
