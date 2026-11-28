#include "engine/renderer/particles.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::renderer {

namespace {

[[nodiscard]] float clampf(float v, float lo, float hi) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : lo;
}

[[nodiscard]] glm::vec2 clamp_range(glm::vec2 r, float lo, float hi) {
    r = glm::vec2(clampf(r.x, lo, hi), clampf(r.y, lo, hi));
    return r.x <= r.y ? r : glm::vec2(r.y, r.x);
}

[[nodiscard]] glm::vec4 column(const glm::mat4& m, int c) { return m[c]; }

// Orthonormal basis around unit `n` (Duff et al., JCGT 2017): t, b, n.
void basis(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
    const float sign = n.z >= 0.0F ? 1.0F : -1.0F;
    const float a = -1.0F / (sign + n.z);
    const float k = n.x * n.y * a;
    t = glm::vec3(1.0F + sign * n.x * n.x * a, sign * k, -sign * n.x);
    b = glm::vec3(k, sign + n.y * n.y * a, -n.y);
}

constexpr float k_two_pi = 2.0F * std::numbers::pi_v<float>;

} // namespace

ParticleEmitterSettings sanitize(const ParticleEmitterSettings& in) {
    ParticleEmitterSettings s = in;
    s.max_particles = std::clamp<std::uint32_t>(s.max_particles, 1, particle_max_per_emitter);
    s.rate = clampf(s.rate, 0.0F, 100000.0F);
    s.burst_count = std::min<std::uint32_t>(s.burst_count, particle_max_per_emitter);
    s.burst_interval = clampf(s.burst_interval, 0.0F, 3600.0F);
    s.lifetime = clamp_range(s.lifetime, 0.01F, 600.0F);
    s.radius = clampf(s.radius, 0.0F, 1000.0F);
    for (int i = 0; i < 3; ++i) s.box_half_extents[i] = clampf(s.box_half_extents[i], 0.0F, 1000.0F);
    s.cone_angle_deg = clampf(s.cone_angle_deg, 0.0F, 90.0F);
    s.spread_deg = clampf(s.spread_deg, 0.0F, 180.0F);
    const float len = glm::length(s.direction);
    s.direction = std::isfinite(len) && len > 1e-6F ? s.direction / len : glm::vec3(0.0F, 1.0F, 0.0F);
    s.speed = clamp_range(s.speed, 0.0F, 1000.0F);
    for (int i = 0; i < 3; ++i) s.gravity[i] = clampf(s.gravity[i], -1000.0F, 1000.0F);
    s.drag = clampf(s.drag, 0.0F, 100.0F);
    s.size = clamp_range(s.size, 0.001F, 1000.0F);
    s.rotation_deg = clamp_range(s.rotation_deg, -3600.0F, 3600.0F);
    s.spin_deg = clamp_range(s.spin_deg, -3600.0F, 3600.0F);

    s.size_keys = std::clamp<std::uint32_t>(s.size_keys, 1, particle_curve_keys);
    for (ParticleSizeKey& k : s.size_curve) {
        k.t = clampf(k.t, 0.0F, 1.0F);
        k.size = clampf(k.size, 0.0F, 1000.0F);
    }
    std::stable_sort(s.size_curve.begin(), s.size_curve.begin() + s.size_keys,
                     [](const ParticleSizeKey& a, const ParticleSizeKey& b) { return a.t < b.t; });
    s.color_keys = std::clamp<std::uint32_t>(s.color_keys, 1, particle_curve_keys);
    for (ParticleColorKey& k : s.color_curve) {
        k.t = clampf(k.t, 0.0F, 1.0F);
        for (int i = 0; i < 3; ++i) k.color[i] = clampf(k.color[i], 0.0F, 1000.0F);
        k.color.a = clampf(k.color.a, 0.0F, 1.0F);
    }
    std::stable_sort(s.color_curve.begin(), s.color_curve.begin() + s.color_keys,
                     [](const ParticleColorKey& a, const ParticleColorKey& b) { return a.t < b.t; });
    s.emissive = clampf(s.emissive, 0.0F, 1000.0F);

    s.flipbook_grid = glm::clamp(s.flipbook_grid, glm::uvec2(1), glm::uvec2(64));
    s.flipbook_frames =
        std::clamp<std::uint32_t>(s.flipbook_frames, 1, s.flipbook_grid.x * s.flipbook_grid.y);
    s.flipbook_fps = clampf(s.flipbook_fps, 0.0F, 240.0F);
    s.soft_distance = clampf(s.soft_distance, 0.0F, 100.0F);
    s.bounce = clampf(s.bounce, 0.0F, 1.0F);
    s.friction = clampf(s.friction, 0.0F, 1.0F);
    return s;
}

std::uint32_t advance_spawn(ParticleSpawnState& state, const ParticleEmitterSettings& settings,
                            float dt) {
    if (!(dt > 0.0F) || !std::isfinite(dt) || !settings.enabled) return 0;
    const double step = static_cast<double>(dt);
    double count = 0.0;
    state.carry += static_cast<double>(settings.rate) * step;
    const double whole = std::floor(state.carry);
    state.carry -= whole;
    count += whole;
    if (settings.burst_count > 0 && !state.bursts_done) {
        // Bursts at 0, interval, 2 interval, ...: each fires in the step
        // whose [time, time + dt) contains it (bounded catch-up).
        for (int guard = 0; guard < 1024 && state.next_burst_s < state.time_s + step; ++guard) {
            count += static_cast<double>(settings.burst_count);
            if (settings.burst_interval > 0.0F) {
                state.next_burst_s += static_cast<double>(settings.burst_interval);
            } else {
                state.bursts_done = true;
                break;
            }
        }
    }
    state.time_s += step;
    const auto n = static_cast<std::uint32_t>(
        std::min(count, static_cast<double>(settings.max_particles)));
    state.spawned += n; // wraps: only an RNG stream index
    return n;
}

// ---- Pool allocator -----------------------------------------------------------

ParticlePoolAllocator::ParticlePoolAllocator(std::uint32_t capacity) : capacity_(capacity) {
    if (capacity_ > 0) free_.push_back({0, capacity_});
}

std::uint32_t ParticlePoolAllocator::allocate(std::uint32_t count) {
    if (count == 0) return UINT32_MAX;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->count < count) continue;
        const std::uint32_t base = it->base;
        it->base += count;
        it->count -= count;
        if (it->count == 0) free_.erase(it);
        return base;
    }
    return UINT32_MAX;
}

void ParticlePoolAllocator::free(std::uint32_t base, std::uint32_t count) {
    if (count == 0 || base >= capacity_ || count > capacity_ - base) return;
    auto it = std::lower_bound(free_.begin(), free_.end(), base,
                               [](const Range& r, std::uint32_t b) { return r.base < b; });
    it = free_.insert(it, Range{base, count});
    // Merge with the next, then the previous range.
    if (auto next = it + 1; next != free_.end() && it->base + it->count == next->base) {
        it->count += next->count;
        free_.erase(next);
    }
    if (it != free_.begin()) {
        auto prev = it - 1;
        if (prev->base + prev->count == it->base) {
            prev->count += it->count;
            free_.erase(it);
        }
    }
}

std::uint32_t ParticlePoolAllocator::free_total() const {
    std::uint32_t total = 0;
    for (const Range& r : free_) total += r.count;
    return total;
}

// ---- GPU records ----------------------------------------------------------------

std::uint32_t particle_sort_size(const ParticleEmitterSettings& settings) {
    if (!settings.sort) return 0;
    return std::max<std::uint32_t>(std::bit_ceil(std::max<std::uint32_t>(settings.max_particles, 1)),
                                   512);
}

EmitterGpu make_emitter_gpu(const ParticleEmitterSettings& settings, const EmitterFrameInfo& info) {
    const ParticleEmitterSettings& s = settings;
    EmitterGpu e;
    glm::mat4 inv = glm::inverse(info.world);
    if (!std::isfinite(inv[0][0]) || std::abs(glm::determinant(info.world)) < 1e-12F) {
        inv = glm::mat4(1.0F);
    }
    for (int c = 0; c < 4; ++c) {
        e.world[c] = column(info.world, c);
        e.inv_world[c] = column(inv, c);
    }
    const bool local = s.space == ParticleSpace::local;
    std::uint32_t flags = 0;
    if (local) flags |= particle_flag_local;
    if (s.collide) flags |= particle_flag_collide;
    if (s.lighting == ParticleLighting::lit) flags |= particle_flag_lit;
    if (s.blend == ParticleBlend::additive) flags |= particle_flag_additive;
    if (info.reset) flags |= particle_flag_reset;
    const std::uint32_t sort_count = particle_sort_size(s);
    if (sort_count > 0) flags |= particle_flag_sort;

    e.range = glm::uvec4(info.pool_base, s.max_particles, info.spawn_count, info.spawn_index);
    e.config = glm::uvec4(static_cast<std::uint32_t>(s.shape), flags, s.seed,
                          s.color_keys | (s.size_keys << 8));
    e.shape = glm::vec4(s.box_half_extents, s.radius);
    const float angle = s.shape == ParticleShape::cone ? s.cone_angle_deg : s.spread_deg;
    e.direction = glm::vec4(s.direction, 1.0F - std::cos(glm::radians(angle)));
    e.speed_life = glm::vec4(s.speed, s.lifetime);
    const glm::vec3 gravity = local ? glm::vec3(inv * glm::vec4(s.gravity, 0.0F)) : s.gravity;
    e.forces = glm::vec4(gravity, 1.0F / (1.0F + s.drag * std::max(info.dt, 0.0F)));
    e.size_rot = glm::vec4(s.size, glm::radians(s.rotation_deg));
    e.spin_hit = glm::vec4(glm::radians(s.spin_deg), s.bounce, 1.0F - s.friction);
    e.flipbook = glm::vec4(static_cast<float>(s.flipbook_grid.x),
                           static_cast<float>(s.flipbook_grid.y),
                           static_cast<float>(s.flipbook_frames), s.flipbook_fps);
    e.render = glm::vec4(s.soft_distance > 0.0F ? 1.0F / s.soft_distance : 0.0F, s.emissive,
                         particle_collision_thickness, 0.0F);
    e.ids = glm::uvec4(info.sort_base, sort_count, info.slot, 0);
    for (std::uint32_t k = 0; k < particle_curve_keys; ++k) {
        // Unused keys repeat the last one (the shader stops at the count).
        const ParticleColorKey& c = s.color_curve[std::min(k, s.color_keys - 1)];
        e.color_value[k] = c.color;
        e.color_time[static_cast<int>(k)] = c.t;
        const ParticleSizeKey& z = s.size_curve[std::min(k, s.size_keys - 1)];
        e.size_value[static_cast<int>(k)] = z.size;
        e.size_time[static_cast<int>(k)] = z.t;
    }
    return e;
}

// ---- CPU references ---------------------------------------------------------------

std::uint32_t particle_hash(std::uint32_t v) {
    const std::uint32_t state = v * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

float particle_random01(std::uint32_t seed, std::uint32_t index, std::uint32_t stream) {
    const std::uint32_t h = particle_hash(particle_hash(index ^ particle_hash(seed)) + stream);
    return static_cast<float>(h >> 8U) * (1.0F / 16777216.0F);
}

ParticleGpu particle_spawn(const EmitterGpu& e, std::uint32_t index) {
    const std::uint32_t seed = e.config.z;
    const auto r = [&](std::uint32_t k) { return particle_random01(seed, index, k); };
    const glm::vec3 axis(e.direction);
    glm::vec3 t;
    glm::vec3 b;
    basis(axis, t, b);

    glm::vec3 pos(0.0F);
    glm::vec3 dir = axis;
    const auto shape = static_cast<ParticleShape>(e.config.x);
    const auto in_cone = [&](float u, float v) {
        const float c = 1.0F - u * e.direction.w;
        const float sn = std::sqrt(std::max(1.0F - c * c, 0.0F));
        const float phi = k_two_pi * v;
        return t * (sn * std::cos(phi)) + b * (sn * std::sin(phi)) + axis * c;
    };
    switch (shape) {
    case ParticleShape::point: dir = in_cone(r(3), r(4)); break;
    case ParticleShape::sphere: {
        const float z = 1.0F - 2.0F * r(0);
        const float rr = std::sqrt(std::max(1.0F - z * z, 0.0F));
        const float phi = k_two_pi * r(1);
        dir = glm::vec3(rr * std::cos(phi), rr * std::sin(phi), z);
        pos = dir * (e.shape.w * std::cbrt(r(2)));
        break;
    }
    case ParticleShape::box:
        pos = (glm::vec3(r(0), r(1), r(2)) * 2.0F - 1.0F) * glm::vec3(e.shape);
        dir = in_cone(r(3), r(4));
        break;
    case ParticleShape::cone: {
        const float rr = e.shape.w * std::sqrt(r(0));
        const float phi = k_two_pi * r(1);
        pos = t * (rr * std::cos(phi)) + b * (rr * std::sin(phi));
        dir = in_cone(r(3), r(4));
        break;
    }
    }
    const float speed = glm::mix(e.speed_life.x, e.speed_life.y, r(5));

    ParticleGpu p;
    if ((e.config.y & particle_flag_local) != 0) {
        p.position = pos;
        p.velocity = dir * speed;
    } else {
        const glm::mat4 world(e.world[0], e.world[1], e.world[2], e.world[3]);
        p.position = glm::vec3(world * glm::vec4(pos, 1.0F));
        const glm::vec3 wd = glm::vec3(world * glm::vec4(dir, 0.0F));
        const float len = glm::length(wd);
        p.velocity = len > 1e-12F ? wd * (speed / len) : glm::vec3(0.0F);
    }
    p.age = 0.0F;
    p.lifetime = glm::mix(e.speed_life.z, e.speed_life.w, r(6));
    p.size = glm::mix(e.size_rot.x, e.size_rot.y, r(7));
    p.rotation = glm::mix(e.size_rot.z, e.size_rot.w, r(8));
    p.spin = glm::mix(e.spin_hit.x, e.spin_hit.y, r(9));
    p.random = particle_hash(index ^ particle_hash(seed + 0x9E3779B9U));
    return p;
}

bool particle_integrate(ParticleGpu& p, const EmitterGpu& e, float dt) {
    p.age += dt;
    if (p.age >= p.lifetime) return false;
    p.velocity = (p.velocity + glm::vec3(e.forces) * dt) * e.forces.w;
    p.position += p.velocity * dt;
    p.rotation += p.spin * dt;
    return true;
}

glm::vec4 particle_color_at(const EmitterGpu& e, float t) {
    const std::uint32_t n = e.config.w & 0xFFU;
    if (n == 0 || t <= e.color_time[0]) return e.color_value[0];
    for (std::uint32_t k = 1; k < n; ++k) {
        const float t1 = e.color_time[static_cast<int>(k)];
        if (t <= t1) {
            const float t0 = e.color_time[static_cast<int>(k - 1)];
            const float f = (t - t0) / std::max(t1 - t0, 1e-6F);
            return glm::mix(e.color_value[k - 1], e.color_value[k], f);
        }
    }
    return e.color_value[n - 1];
}

float particle_size_at(const EmitterGpu& e, float t) {
    const std::uint32_t n = (e.config.w >> 8U) & 0xFFU;
    if (n == 0 || t <= e.size_time[0]) return e.size_value[0];
    for (std::uint32_t k = 1; k < n; ++k) {
        const float t1 = e.size_time[static_cast<int>(k)];
        if (t <= t1) {
            const float t0 = e.size_time[static_cast<int>(k - 1)];
            const float f = (t - t0) / std::max(t1 - t0, 1e-6F);
            return glm::mix(e.size_value[static_cast<int>(k - 1)],
                            e.size_value[static_cast<int>(k)], f);
        }
    }
    return e.size_value[static_cast<int>(n - 1)];
}

float wboit_weight(float alpha, float view_depth) {
    const float a = view_depth / 5.0F;
    const float b = view_depth / 200.0F;
    const float b2 = b * b;
    return alpha * std::clamp(10.0F / (1e-5F + a * a + b2 * b2 * b2), 1e-2F, 3e3F);
}

} // namespace engine::renderer
