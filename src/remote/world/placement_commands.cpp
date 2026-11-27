// Entity bounds and world.place_on_ground (ROADMAP A9, issue #40).
//
// Bounds come from what each entity draws (engine/scene/bounds.hpp) through
// exact world matrices (world_matrix composes the local transforms, so edits
// earlier in a batch count). Placement rests the entity's bounds on the
// surfaces below it (surface_query.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/bounds.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

std::optional<asset::Aabb> subtree_world_bounds(const Context& ctx, entt::entity e,
                                                bool include_terrain) {
    const entt::registry& r = ctx.scene->registry();
    const std::optional<TerrainRef> terrain =
        include_terrain ? find_terrain(r) : std::optional<TerrainRef>{};
    asset::Aabb box = scene::empty_bounds();
    for (const entt::entity c : subtree(r, e)) {
        if (const auto local = scene::local_draw_bounds(r, c)) {
            scene::merge_bounds(box, scene::transform_bounds(*local, world_matrix(r, c)));
        }
        if (terrain && terrain->entity == c && !terrain->regenerating &&
            ctx.services.terrain_status) {
            const TerrainStatus s = ctx.services.terrain_status();
            if (s.has_bounds) {
                scene::merge_bounds(box, {terrain->anchor + s.bounds_min,
                                          terrain->anchor + s.bounds_max});
            }
        }
    }
    if (scene::bounds_empty(box)) return std::nullopt;
    return box;
}

std::optional<asset::Aabb> subtree_local_bounds(const Context& ctx, entt::entity e) {
    const entt::registry& r = ctx.scene->registry();
    const glm::mat4 world = world_matrix(r, e);
    if (std::abs(glm::determinant(world)) < 1e-12F) return std::nullopt; // zero scale
    const glm::mat4 to_local = glm::inverse(world);
    asset::Aabb box = scene::empty_bounds();
    for (const entt::entity c : subtree(r, e)) {
        if (const auto local = scene::local_draw_bounds(r, c)) {
            const glm::mat4 m = c == e ? glm::mat4(1.0F) : to_local * world_matrix(r, c);
            scene::merge_bounds(box, scene::transform_bounds(*local, m));
        }
    }
    if (scene::bounds_empty(box)) return std::nullopt;
    return box;
}

Json bounds_json(const std::optional<asset::Aabb>& box) {
    if (!box) return nullptr;
    return Json{{"min", vec3_json(box->min)}, {"max", vec3_json(box->max)}};
}

namespace {

// Sets `e`'s world matrix (through its parent's space) and refreshes the
// world transforms below it.
void set_world_matrix(entt::registry& r, entt::entity e, const glm::mat4& world) {
    const entt::entity parent = parent_of(r, e);
    const glm::mat4 parent_world = parent == entt::null ? glm::mat4(1.0F) : world_matrix(r, parent);
    r.get<scene::Transform>(e).local = glm::inverse(parent_world) * world;
    refresh_world(r, e);
}

// Rotation taking unit `from` onto unit `to`, limited to `max_rad`.
[[nodiscard]] glm::quat tilt_between(const glm::vec3& from, const glm::vec3& to, float max_rad) {
    const float angle = std::acos(glm::clamp(glm::dot(from, to), -1.0F, 1.0F));
    if (angle < 1e-5F) return glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    glm::vec3 axis = glm::cross(from, to);
    if (glm::length(axis) < 1e-6F) axis = glm::vec3(1.0F, 0.0F, 0.0F); // opposite: any axis
    return glm::angleAxis(std::min(angle, max_rad), glm::normalize(axis));
}

struct Support {
    float          y = 0.0F;
    const Surface* centre = nullptr; // the surface under the centre sample
};

// The height the footprint `box` rests at: the lowest ground sample (a rock
// on a slope sinks in on the uphill side rather than floating) or the highest
// object sample (a crate overhanging a table edge stays on the table),
// whichever is higher.
[[nodiscard]] std::optional<float> rest_height(const std::array<SurfaceSample, 9>& samples) {
    std::optional<float> ground;
    std::optional<float> object;
    for (const SurfaceSample& s : samples) {
        if (s.ground) ground = ground ? std::min(*ground, s.ground->y) : s.ground->y;
        if (s.object) object = object ? std::max(*object, s.object->y) : s.object->y;
    }
    if (ground && object) return std::max(*ground, *object);
    return ground ? ground : object;
}

// The surface a sample stands on (the higher one).
[[nodiscard]] const Surface* top_surface(const SurfaceSample& s) {
    if (s.ground && s.object) return s.object->y >= s.ground->y ? &*s.object : &*s.ground;
    return s.ground ? &*s.ground : (s.object ? &*s.object : nullptr);
}

[[nodiscard]] std::unexpected<RpcError> nothing_below(const glm::vec3& p) {
    return rpc_fail(rpc_code::failed,
                    "nothing to rest on below (" + std::to_string(p.x) + ", " +
                        std::to_string(p.z) +
                        "): no loaded terrain and no collider under the entity's footprint (give "
                        "the surface a `collider`, or check terrain.status)");
}

RpcResult place_on_ground(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    glm::vec2 at(0.0F);
    const Json* at_in = f.raw("at");
    const bool has_at = at_in != nullptr && !at_in->is_null();
    if (has_at && (!at_in->is_array() || at_in->size() != 2 ||
                   !core::JsonFields::read_float((*at_in)[0], at.x) ||
                   !core::JsonFields::read_float((*at_in)[1], at.y))) {
        f.fail("at", "expected [x, z] (finite numbers): where to place the entity horizontally");
    }
    bool align = false;
    float max_tilt_deg = 30.0F;
    float offset = 0.0F;
    f.boolean("align_to_normal", align)
        .number("max_tilt_deg", max_tilt_deg, 0.0F, 90.0F)
        .number("offset", offset, -1000.0F, 1000.0F);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    if (!r.all_of<scene::Transform>(*e)) {
        return bad_params("entity: has no transform, so it cannot be placed");
    }

    const std::vector<entt::entity> self = subtree(r, *e);
    const std::unordered_set<entt::entity> ignore(self.begin(), self.end());
    const std::optional<TerrainRef> terrain = find_terrain(r);
    if (terrain && ignore.contains(terrain->entity)) {
        return bad_params("entity: the terrain (or an ancestor of it) cannot be placed on itself");
    }

    // The placement moves the entity in steps (at, tilt, rest); a failure at
    // any step restores the transform it had, so an error changes nothing.
    const glm::mat4 saved_local = r.get<scene::Transform>(*e).local;
    const auto place = [&]() -> RpcResult {
        glm::mat4 world = world_matrix(r, *e);
        if (has_at) {
            world[3].x = at.x;
            world[3].z = at.y;
            set_world_matrix(r, *e, world);
        }
        const auto bounds_now = [&] {
            const glm::vec3 p(world_matrix(r, *e)[3]);
            return subtree_world_bounds(ctx, *e, false).value_or(asset::Aabb{p, p});
        };

        // 1. Tilt to the surface under the entity's centre.
        float tilt_deg = 0.0F;
        if (align) {
            const asset::Aabb box = bounds_now();
            const glm::vec3 c = 0.5F * (box.min + box.max);
            const SurfaceSample s = sample_surfaces(ctx, terrain, ignore, c.x, c.z, box.max.y + 1e-3F);
            const Surface* surface = top_surface(s);
            if (surface == nullptr) return nothing_below(c);
            const glm::vec3 up = glm::normalize(glm::vec3(world[1]));
            const glm::quat q = tilt_between(up, glm::normalize(surface->normal),
                                             glm::radians(max_tilt_deg));
            const glm::vec3 pivot(world[3]);
            world = glm::translate(glm::mat4(1.0F), pivot) * glm::mat4_cast(q) *
                    glm::translate(glm::mat4(1.0F), -pivot) * world;
            set_world_matrix(r, *e, world);
            tilt_deg = glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(glm::vec3(world[1])),
                                                                  glm::vec3(0.0F, 1.0F, 0.0F)),
                                                         -1.0F, 1.0F)));
        }

        // 2. Rest the bounds on the surfaces under a 3x3 footprint grid.
        const asset::Aabb box = bounds_now();
        std::array<SurfaceSample, 9> samples;
        for (int i = 0; i < 9; ++i) {
            const float u = static_cast<float>(i % 3) * 0.5F;
            const float v = static_cast<float>(i / 3) * 0.5F;
            samples[static_cast<std::size_t>(i)] =
                sample_surfaces(ctx, terrain, ignore, glm::mix(box.min.x, box.max.x, u),
                                glm::mix(box.min.z, box.max.z, v), box.max.y + 1e-3F);
        }
        const SurfaceSample& centre = samples[4];
        float dy = 0.0F;
        if (align) {
            // Tilted: the bottom centre of the entity's own bounds sits on the
            // surface under it (the footprint's corners follow the slope).
            const auto local = subtree_local_bounds(ctx, *e);
            const glm::vec3 bottom_local =
                local ? glm::vec3(0.5F * (local->min.x + local->max.x), local->min.y,
                                  0.5F * (local->min.z + local->max.z))
                      : glm::vec3(0.0F);
            const glm::vec3 bottom(world * glm::vec4(bottom_local, 1.0F));
            const SurfaceSample s = sample_surfaces(ctx, terrain, ignore, bottom.x, bottom.z,
                                                    box.max.y + 1e-3F);
            const Surface* surface = top_surface(s);
            if (surface == nullptr) return nothing_below(bottom);
            dy = surface->y - bottom.y;
        } else {
            const std::optional<float> rest = rest_height(samples);
            if (!rest) return nothing_below(0.5F * (box.min + box.max));
            dy = *rest - box.min.y;
        }
        dy += offset;
        world = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, dy, 0.0F)) * world;
        set_world_matrix(r, *e, world);

        const Surface* under = top_surface(centre);
        const asset::Aabb placed = bounds_now();
        Json surface = nullptr;
        if (under != nullptr) {
            surface = Json{{"kind", under->kind},
                           {"entity", under->entity == entt::null ? Json(nullptr)
                                                                  : entity_json(under->entity)},
                           {"y", under->y},
                           {"normal", vec3_json(under->normal)}};
        }
        return Json{{"id", entity_json(*e)},
                    {"position", vec3_json(glm::vec3(world[3]))},
                    {"world_bounds", bounds_json(placed)},
                    {"moved_y", dy},
                    {"tilt_deg", tilt_deg},
                    {"surface", std::move(surface)}};
    };
    RpcResult result = place();
    if (!result) {
        r.get<scene::Transform>(*e).local = saved_local;
        refresh_world(r, *e);
    }
    return result;
}

} // namespace

void register_placement_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method(
        "world.place_on_ground",
        "Rest an entity (with its children) on what is below it: {entity, at?:[x, z], "
        "align_to_normal?=false, max_tilt_deg?=30, offset?=0} -> {id, position, world_bounds, "
        "moved_y, tilt_deg, surface:{kind, entity, y, normal}}. Surfaces: terrain and colliders.",
        [ctx](const Json& p) { return place_on_ground(*ctx, p); });
}

} // namespace engine::remote::world
