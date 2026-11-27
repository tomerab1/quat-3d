#include "picking.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <entt/entity/registry.hpp>

#include "engine/asset/mesh_asset.hpp"
#include "engine/scene/components.hpp"

namespace engine::editor {

namespace {

// Slab test: entry/exit parameters of the ray against [lo, hi], or nullopt
// when it misses. Parameters may be negative (behind the origin).
[[nodiscard]] std::optional<std::pair<float, float>>
ray_slabs(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& lo,
          const glm::vec3& hi) {
    float t_enter = -std::numeric_limits<float>::max();
    float t_exit = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8F) {
            if (origin[i] < lo[i] || origin[i] > hi[i]) return std::nullopt;
            continue;
        }
        float t1 = (lo[i] - origin[i]) / dir[i];
        float t2 = (hi[i] - origin[i]) / dir[i];
        if (t1 > t2) std::swap(t1, t2);
        t_enter = std::max(t_enter, t1);
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit) return std::nullopt;
    }
    return std::pair{t_enter, t_exit};
}

} // namespace

std::optional<float> pick_distance(const glm::vec3& origin, const glm::vec3& dir,
                                   const glm::mat4& world, const asset::Aabb& bounds) {
    // Test in the mesh's local space (the box is axis-aligned there).
    const glm::mat4 inv_world = glm::inverse(world);
    const glm::vec3 lo = glm::vec3(inv_world * glm::vec4(origin, 1.0F));
    const glm::vec3 ld_raw = glm::vec3(inv_world * glm::vec4(dir, 0.0F));
    const float ld_len = glm::length(ld_raw);
    if (!std::isfinite(ld_len) || ld_len < 1e-12F || !std::isfinite(lo.x + lo.y + lo.z)) {
        return std::nullopt; // degenerate (zero-scale) transform
    }
    const glm::vec3 ld = ld_raw / ld_len;
    const auto slabs = ray_slabs(lo, ld, bounds.min, bounds.max);
    if (!slabs || slabs->second < 0.0F) return std::nullopt; // missed, or behind
    // Outside: where the ray enters. Inside: where it leaves.
    const float t = slabs->first >= 0.0F ? slabs->first : slabs->second;
    const glm::vec3 hit_world = glm::vec3(world * glm::vec4(lo + ld * t, 1.0F));
    return glm::length(hit_world - origin);
}

entt::entity pick_entity(const entt::registry& registry, const glm::vec3& origin,
                         const glm::vec3& dir) {
    float best = std::numeric_limits<float>::max();
    entt::entity hit = entt::null;
    for (auto [e, transform, mr] :
         registry.view<const scene::Transform, const scene::MeshRenderer>().each()) {
        if (!mr.mesh.valid() || !mr.mesh.is_loaded()) continue;
        const std::optional<float> d = pick_distance(origin, dir, transform.world, mr.mesh->bounds);
        if (d && *d < best) {
            best = *d;
            hit = e;
        }
    }
    return hit;
}

} // namespace engine::editor
