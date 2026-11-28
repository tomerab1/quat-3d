// Surfaces under a point (ROADMAP A9, issue #40): what world.place_on_ground
// rests an entity on. Two kinds:
//   ground   — the terrain (sampled analytically) and, while a physics world
//              runs, ownerless static bodies (streamed terrain height fields,
//              demo floors);
//   objects  — collider shapes of other entities: the physics world's bodies
//              and characters while it runs, otherwise (edit mode) each
//              Collider component intersected analytically with the same
//              world-scaled shape the physics system would build.

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr float k_ray_length = 100'000.0F;
constexpr int   k_max_self_skips = 16;

struct RayHit {
    float     t = 0.0F;
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
};

// Ray (unit `d`) against an oriented box: centre c, rotation r, half extents h.
[[nodiscard]] std::optional<RayHit> ray_box(const glm::vec3& o, const glm::vec3& d,
                                            const glm::vec3& c, const glm::mat3& r,
                                            const glm::vec3& h) {
    const glm::mat3 rt = glm::transpose(r);
    const glm::vec3 lo = rt * (o - c);
    const glm::vec3 ld = rt * d;
    float t_near = -k_ray_length;
    float t_far = k_ray_length;
    int axis = -1;
    float sign = 1.0F;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(ld[i]) < 1e-9F) {
            if (lo[i] < -h[i] || lo[i] > h[i]) return std::nullopt;
            continue;
        }
        float t0 = (-h[i] - lo[i]) / ld[i];
        float t1 = (h[i] - lo[i]) / ld[i];
        float s = -1.0F; // entering through the -h face
        if (t0 > t1) {
            std::swap(t0, t1);
            s = 1.0F;
        }
        if (t0 > t_near) {
            t_near = t0;
            axis = i;
            sign = s;
        }
        t_far = std::min(t_far, t1);
        if (t_near > t_far) return std::nullopt;
    }
    if (axis < 0 || t_near < 0.0F) return std::nullopt; // origin inside / box behind
    glm::vec3 n(0.0F);
    n[axis] = sign;
    return RayHit{t_near, glm::normalize(r * n)};
}

[[nodiscard]] std::optional<RayHit> ray_sphere(const glm::vec3& o, const glm::vec3& d,
                                               const glm::vec3& c, float radius) {
    const glm::vec3 oc = o - c;
    const float b = glm::dot(oc, d);
    const float h = b * b - (glm::dot(oc, oc) - radius * radius);
    if (h < 0.0F) return std::nullopt;
    const float t = -b - std::sqrt(h);
    if (t < 0.0F) return std::nullopt;
    return RayHit{t, glm::normalize(o + d * t - c)};
}

// Capsule: the segment a-b swept by `radius`.
[[nodiscard]] std::optional<RayHit> ray_capsule(const glm::vec3& o, const glm::vec3& d,
                                                const glm::vec3& a, const glm::vec3& b,
                                                float radius) {
    const glm::vec3 ba = b - a;
    const glm::vec3 oa = o - a;
    const float baba = glm::dot(ba, ba);
    const float bard = glm::dot(ba, d);
    const float baoa = glm::dot(ba, oa);
    std::optional<RayHit> best;
    const auto keep = [&best](const std::optional<RayHit>& h) {
        if (h && (!best || h->t < best->t)) best = h;
    };
    const float k2 = baba - bard * bard;
    if (k2 > 1e-8F * std::max(baba, 1e-8F)) { // the cylinder (not parallel to the ray)
        const float k1 = baba * glm::dot(oa, d) - baoa * bard;
        const float k0 = baba * glm::dot(oa, oa) - baoa * baoa - radius * radius * baba;
        const float h = k1 * k1 - k2 * k0;
        if (h >= 0.0F) {
            const float t = (-k1 - std::sqrt(h)) / k2;
            const float y = baoa + t * bard;
            if (t >= 0.0F && y > 0.0F && y < baba) {
                const glm::vec3 p = o + d * t;
                const glm::vec3 axis_point = a + ba * (y / baba);
                keep(RayHit{t, glm::normalize(p - axis_point)});
            }
        }
    }
    keep(ray_sphere(o, d, a, radius));
    keep(ray_sphere(o, d, b, radius));
    return best;
}

// Edit mode: the ray against one entity's collider, shaped as physics_system
// builds it (world scale baked in, rotation scale-free, offset scaled).
[[nodiscard]] std::optional<RayHit> ray_collider(const glm::vec3& o, const glm::vec3& d,
                                                 const glm::mat4& world, const scene::Collider& col) {
    const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                          glm::length(glm::vec3(world[2])));
    if (scale.x < 1e-12F || scale.y < 1e-12F || scale.z < 1e-12F) return std::nullopt;
    const glm::mat3 rot(glm::vec3(world[0]) / scale.x, glm::vec3(world[1]) / scale.y,
                        glm::vec3(world[2]) / scale.z);
    const glm::vec3 centre = glm::vec3(world[3]) + rot * (scale * col.offset);
    switch (col.shape) {
    case scene::ColliderShape::box:
        return ray_box(o, d, centre, rot, col.half_extents * scale);
    case scene::ColliderShape::sphere:
        return ray_sphere(o, d, centre, col.half_extents.x * std::max({scale.x, scale.y, scale.z}));
    case scene::ColliderShape::capsule: {
        const glm::vec3 axis = rot[1] * (col.half_extents.y * scale.y);
        return ray_capsule(o, d, centre - axis, centre + axis,
                           col.half_extents.x * std::max(scale.x, scale.z));
    }
    }
    return std::nullopt;
}

} // namespace

SurfaceSample sample_surfaces(const Context& ctx, const std::optional<TerrainRef>& terrain,
                              const std::unordered_set<entt::entity>& ignore, float x, float z,
                              float top_y) {
    SurfaceSample out;
    if (terrain) {
        if (const auto h = terrain_height_at(ctx, *terrain, x, z)) {
            // Central differences over ~half a metre give the normal.
            constexpr float k_step = 0.5F;
            const auto hx0 = terrain_height_at(ctx, *terrain, x - k_step, z);
            const auto hx1 = terrain_height_at(ctx, *terrain, x + k_step, z);
            const auto hz0 = terrain_height_at(ctx, *terrain, x, z - k_step);
            const auto hz1 = terrain_height_at(ctx, *terrain, x, z + k_step);
            const float dx = (hx1.value_or(*h) - hx0.value_or(*h)) /
                             ((hx1 ? k_step : 0.0F) + (hx0 ? k_step : 0.0F) + 1e-12F);
            const float dz = (hz1.value_or(*h) - hz0.value_or(*h)) /
                             ((hz1 ? k_step : 0.0F) + (hz0 ? k_step : 0.0F) + 1e-12F);
            out.ground = Surface{*h, glm::normalize(glm::vec3(-dx, 1.0F, -dz)), "terrain",
                                 terrain->entity};
        }
    }

    const glm::vec3 origin(x, top_y, z);
    const glm::vec3 down(0.0F, -1.0F, 0.0F);
    const entt::registry& r = ctx.scene->registry();
    const physics::PhysicsWorld* world =
        ctx.services.physics_world ? ctx.services.physics_world() : nullptr;
    if (world != nullptr) {
        // The closest hit that is not the entity being placed (re-cast below it).
        glm::vec3 from = origin;
        for (int i = 0; i < k_max_self_skips; ++i) {
            const auto hit = world->cast_ray(from, down, k_ray_length);
            if (!hit) break;
            const auto owner = static_cast<entt::entity>(hit->owner);
            const bool owned = hit->owner != physics::PhysicsWorld::no_owner;
            if (owned && ignore.contains(owner)) {
                from = hit->point - glm::vec3(0.0F, 1e-3F, 0.0F);
                continue;
            }
            const Surface s{hit->point.y, hit->normal,
                            hit->character != physics::PhysicsWorld::invalid_body ? "character"
                            : owned                                              ? "body"
                                                                                 : "static_world",
                            owned && r.valid(owner) ? owner : entt::entity{entt::null}};
            if (owned) {
                out.object = s;
            } else if (!out.ground || s.y > out.ground->y) {
                out.ground = s;
            }
            break;
        }
        return out;
    }

    for (const auto [e, col, t] : r.view<const scene::Collider, const scene::Transform>().each()) {
        if (col.is_sensor || ignore.contains(e)) continue;
        const auto hit = ray_collider(origin, down, world_matrix(r, e), col);
        if (!hit) continue;
        const float y = top_y - hit->t;
        if (!out.object || y > out.object->y) out.object = Surface{y, hit->normal, "collider", e};
    }
    return out;
}

} // namespace engine::remote::world
