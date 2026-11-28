// Physics queries of a running simulation (ROADMAP A10.1):
// physics.raycast_all, physics.overlap and physics.shape_cast, filtered by
// collision layer. The contract is docs/remote-control.md ("Physics control").

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

using physics::PhysicsWorld;

constexpr float k_max_distance = 100'000.0F;
constexpr float k_max_shape_size = 1000.0F;
constexpr float k_max_coordinate = 1.0e6F;

constexpr core::EnumName<PhysicsWorld::QueryShape> k_shapes[] = {
    {"sphere", PhysicsWorld::QueryShape::sphere},
    {"box", PhysicsWorld::QueryShape::box},
    {"capsule", PhysicsWorld::QueryShape::capsule}};

// Euler degrees (pitch about X, yaw about Y, roll about Z) composed Y * X * Z,
// as the transform component does.
[[nodiscard]] glm::quat rotation_from_degrees(const glm::vec3& deg) {
    const glm::vec3 r = glm::radians(deg);
    return glm::normalize(glm::angleAxis(r.y, glm::vec3(0.0F, 1.0F, 0.0F)) *
                          glm::angleAxis(r.x, glm::vec3(1.0F, 0.0F, 0.0F)) *
                          glm::angleAxis(r.z, glm::vec3(0.0F, 0.0F, 1.0F)));
}

// The query-shape fields: `shape` and the dimensions it uses. A dimension the
// shape does not use is an error (a box has no radius).
void read_shape(core::JsonFields& f, PhysicsWorld::ShapeDesc& d, bool& has_shape) {
    has_shape = f.has("shape");
    f.enumeration("shape", d.type, k_shapes);
    const bool box = d.type == PhysicsWorld::QueryShape::box;
    const bool capsule = d.type == PhysicsWorld::QueryShape::capsule;
    if (f.has("radius") && box) f.fail("radius", "a box takes half_extents, not radius");
    if (f.has("half_extents") && !box) {
        f.fail("half_extents", "only a box takes half_extents (spheres and capsules take radius)");
    }
    if (f.has("half_height") && !capsule) f.fail("half_height", "only a capsule takes half_height");
    f.number("radius", d.radius, 1e-3F, k_max_shape_size);
    f.vec3("half_extents", d.half_extents);
    f.number("half_height", d.half_height, 0.0F, k_max_shape_size);
    if (box && (glm::any(glm::lessThan(d.half_extents, glm::vec3(1e-3F))) ||
                glm::any(glm::greaterThan(d.half_extents, glm::vec3(k_max_shape_size))))) {
        f.fail("half_extents", "components must be in [0.001, 1000] metres");
    }
}

void read_rotation(core::JsonFields& f, glm::quat& out) {
    glm::vec3 deg{0.0F};
    f.vec3("rotation", deg);
    out = rotation_from_degrees(deg);
}

void read_point(core::JsonFields& f, std::string_view key, glm::vec3& out) {
    f.vec3(key, out);
    if (glm::any(glm::greaterThan(glm::abs(out), glm::vec3(k_max_coordinate)))) {
        f.fail(key, "coordinates must be within +-1e6 metres");
    }
}

[[nodiscard]] Json hit_json(const entt::registry& r, const PhysicsWorld::QueryHit& h, bool cast,
                            bool overlap) {
    Json out = Json::object();
    put_owner(r, h.owner, out);
    if (h.character != PhysicsWorld::invalid_body) {
        out["kind"] = "character";
    } else {
        out["kind"] = h.owner == PhysicsWorld::no_owner ? "static_world" : "body";
    }
    out["layer"] = std::string(layer_name(h.layer));
    out["point"] = vec3_json(h.point);
    out["normal"] = vec3_json(h.normal);
    if (cast) {
        out["fraction"] = h.fraction;
        out["distance"] = h.distance;
    }
    if (overlap || (cast && h.fraction <= 0.0F)) out["penetration"] = h.penetration;
    return out;
}

[[nodiscard]] Json hits_json(const entt::registry& r, const std::vector<PhysicsWorld::QueryHit>& hits,
                             bool cast, bool overlap) {
    Json list = Json::array();
    for (const PhysicsWorld::QueryHit& h : hits) list.push_back(hit_json(r, h, cast, overlap));
    const std::size_t n = list.size();
    return Json{{"hits", std::move(list)}, {"count", n}};
}

RpcResult physics_raycast_all(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    glm::vec3 origin{0.0F};
    glm::vec3 direction{0.0F};
    float max_distance = 1000.0F;
    int max_hits = 32;
    const bool has_origin = f.has("origin");
    const bool has_direction = f.has("direction");
    read_point(f, "origin", origin);
    f.vec3("direction", direction)
        .number("max_distance", max_distance, 1e-3F, k_max_distance)
        .integer("max_hits", max_hits, 1, static_cast<int>(PhysicsWorld::max_query_hits));
    const Json* layers = f.raw("layers");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_origin) return bad_params("origin: required (world-space [x, y, z])");
    if (!has_direction || !(glm::length(direction) > 1e-6F)) {
        return bad_params("direction: required, a non-zero [x, y, z] (need not be normalized)");
    }
    physics::LayerMask mask = physics::solid_layers;
    if (auto ok = read_layer_mask(layers, mask); !ok) return std::unexpected(ok.error());
    auto world = running_physics(ctx);
    if (!world) return std::unexpected(world.error());
    const auto hits = (*world)->cast_ray_all(origin, direction, max_distance, mask,
                                             static_cast<std::size_t>(max_hits));
    return hits_json(ctx.scene->registry(), hits, true, false);
}

RpcResult physics_overlap(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    PhysicsWorld::ShapeDesc shape;
    bool has_shape = false;
    read_shape(f, shape, has_shape);
    glm::vec3 position{0.0F};
    const bool has_position = f.has("position");
    read_point(f, "position", position);
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    read_rotation(f, rotation);
    int max_hits = 32;
    f.integer("max_hits", max_hits, 1, static_cast<int>(PhysicsWorld::max_query_hits));
    const Json* layers = f.raw("layers");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_shape) return bad_params("shape: required, one of \"sphere\", \"box\", \"capsule\"");
    if (!has_position) return bad_params("position: required (world-space [x, y, z])");
    physics::LayerMask mask = physics::solid_layers;
    if (auto ok = read_layer_mask(layers, mask); !ok) return std::unexpected(ok.error());
    auto world = running_physics(ctx);
    if (!world) return std::unexpected(world.error());
    const auto hits = (*world)->overlap(shape, position, rotation, mask,
                                        static_cast<std::size_t>(max_hits));
    return hits_json(ctx.scene->registry(), hits, false, true);
}

RpcResult physics_shape_cast(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    PhysicsWorld::ShapeDesc shape;
    bool has_shape = false;
    read_shape(f, shape, has_shape);
    glm::vec3 from{0.0F};
    glm::vec3 to{0.0F};
    const bool has_from = f.has("from");
    const bool has_to = f.has("to");
    read_point(f, "from", from);
    read_point(f, "to", to);
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    read_rotation(f, rotation);
    int max_hits = 32;
    f.integer("max_hits", max_hits, 1, static_cast<int>(PhysicsWorld::max_query_hits));
    const Json* layers = f.raw("layers");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_shape) return bad_params("shape: required, one of \"sphere\", \"box\", \"capsule\"");
    if (!has_from || !has_to) return bad_params("from, to: both required (world-space [x, y, z])");
    const float length = glm::length(to - from);
    if (!(length > 1e-4F)) {
        return bad_params("to: must differ from `from` (use physics.overlap for a shape at rest)");
    }
    if (length > k_max_distance) return bad_params("to: the cast is longer than 100000 metres");
    physics::LayerMask mask = physics::solid_layers;
    if (auto ok = read_layer_mask(layers, mask); !ok) return std::unexpected(ok.error());
    auto world = running_physics(ctx);
    if (!world) return std::unexpected(world.error());
    const auto hits = (*world)->cast_shape(shape, from, to, rotation, mask,
                                           static_cast<std::size_t>(max_hits));
    return hits_json(ctx.scene->registry(), hits, true, false);
}

} // namespace

void register_physics_query_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("physics.raycast_all",
                      "Every hit along a ray, nearest first: {origin, direction, max_distance?=1000, "
                      "layers?, max_hits?=32} -> {hits:[{entity, name, kind, layer, point, normal, "
                      "fraction, distance}], count}.",
                      [ctx](const Json& p) { return physics_raycast_all(*ctx, p); });
    server.add_method("physics.overlap",
                      "Bodies a shape overlaps, deepest first: {shape: sphere|box|capsule, radius? | "
                      "half_extents?, half_height?, position, rotation?, layers?, max_hits?} -> "
                      "{hits:[{entity, name, kind, layer, point, normal, penetration}], count}.",
                      [ctx](const Json& p) { return physics_overlap(*ctx, p); });
    server.add_method("physics.shape_cast",
                      "Sweep a shape from `from` to `to`, nearest hits first: {shape, radius? | "
                      "half_extents?, half_height?, from, to, rotation?, layers?, max_hits?} -> "
                      "{hits:[{entity, name, kind, layer, point, normal, fraction, distance}], count}.",
                      [ctx](const Json& p) { return physics_shape_cast(*ctx, p); });
}

} // namespace engine::remote::world
