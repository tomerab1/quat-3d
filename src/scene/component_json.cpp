// Component JSON codec — see include/engine/scene/component_json.hpp.

#define GLM_ENABLE_EXPERIMENTAL
#include "engine/scene/component_json.hpp"

#include <array>
#include <cmath>
#include <string>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

using json = nlohmann::json;
using core::EnumName;
using core::JsonFields;

constexpr std::array k_components = {
    ComponentInfo{"transform", "Local position / rotation (Euler degrees, YXZ) / scale", false, true},
    ComponentInfo{"primitive", "Procedural shape (cube, sphere, plane, cylinder, cone, capsule) with a PBR colour", true, true},
    ComponentInfo{"point_light", "Point light at the entity position", true, true},
    ComponentInfo{"dir_light", "Directional light (the sun)", true, true},
    ComponentInfo{"camera", "Perspective camera", true, true},
    ComponentInfo{"collider", "Physics collision shape (box, sphere, capsule)", true, true},
    ComponentInfo{"rigidbody", "Physics body: static, kinematic or dynamic", true, true},
    ComponentInfo{"character", "Capsule character controller", true, true},
    ComponentInfo{"nav_agent", "Navmesh path-following agent", true, true},
    ComponentInfo{"behavior", "Behaviour tree (JSON source)", true, true},
    ComponentInfo{"patrol", "Patrol route the nav agent walks", true, true},
    ComponentInfo{"terrain", "Procedural terrain tile / streamed world", true, true},
    ComponentInfo{"net_replicated", "Replicate this entity's transform to network clients", true, true},
    ComponentInfo{"animator", "Animation playback of a glTF clip (time, speed, looping)", false, true},
    ComponentInfo{"mesh", "glTF mesh provenance (read-only)", false, false},
};

constexpr EnumName<ColliderShape> k_collider_shapes[] = {
    {"box", ColliderShape::box}, {"sphere", ColliderShape::sphere},
    {"capsule", ColliderShape::capsule}};
constexpr EnumName<BodyMotion> k_motions[] = {{"static", BodyMotion::static_body},
                                              {"kinematic", BodyMotion::kinematic},
                                              {"dynamic", BodyMotion::dynamic}};
constexpr EnumName<PrimitiveShape> k_primitive_shapes[] = {
    {"cube", PrimitiveShape::cube},         {"sphere", PrimitiveShape::sphere},
    {"plane", PrimitiveShape::plane},       {"cylinder", PrimitiveShape::cylinder},
    {"cone", PrimitiveShape::cone},         {"capsule", PrimitiveShape::capsule}};

template <typename E, std::size_t N>
[[nodiscard]] std::string enum_name(const EnumName<E> (&names)[N], E value) {
    for (const auto& n : names) {
        if (n.value == value) return std::string(n.name);
    }
    return "unknown";
}

[[nodiscard]] json vec3_json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
[[nodiscard]] json vec4_json(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

// ---- Transform ----------------------------------------------------------

struct Trs {
    glm::vec3 position{0.0F};
    glm::vec3 rotation_deg{0.0F}; // (pitch about X, yaw about Y, roll about Z)
    glm::vec3 scale{1.0F};
};

[[nodiscard]] Trs decompose(const glm::mat4& m) {
    Trs t;
    t.position = glm::vec3(m[3]);
    t.scale = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
               glm::length(glm::vec3(m[2]))};
    if (glm::determinant(glm::mat3(m)) < 0.0F) t.scale.x = -t.scale.x; // mirrored
    glm::mat4 rot(1.0F);
    for (int c = 0; c < 3; ++c) {
        const float s = t.scale[c];
        rot[c] = std::abs(s) > 1e-12F ? glm::vec4(glm::vec3(m[c]) / s, 0.0F)
                                      : glm::vec4(0.0F);
    }
    float yaw = 0.0F;
    float pitch = 0.0F;
    float roll = 0.0F;
    glm::extractEulerAngleYXZ(rot, yaw, pitch, roll);
    t.rotation_deg = glm::degrees(glm::vec3(pitch, yaw, roll));
    return t;
}

[[nodiscard]] glm::mat4 compose(const Trs& t) {
    const glm::vec3 r = glm::radians(t.rotation_deg);
    return glm::translate(glm::mat4(1.0F), t.position) * glm::eulerAngleYXZ(r.y, r.x, r.z) *
           glm::scale(glm::mat4(1.0F), t.scale);
}

[[nodiscard]] json encode_transform(const Transform& t, bool as_matrix) {
    if (as_matrix) {
        json out = json::array();
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) out.push_back(t.local[c][r]);
        }
        return out;
    }
    const Trs trs = decompose(t.local);
    return {{"position", vec3_json(trs.position)},
            {"rotation", vec3_json(trs.rotation_deg)},
            {"scale", vec3_json(trs.scale)},
            {"world_position", vec3_json(glm::vec3(t.world[3]))}};
}

[[nodiscard]] std::expected<void, core::Error> decode_transform(Transform& t, const json& v,
                                                                bool strict) {
    if (v.is_array()) { // exact matrix (scene files)
        bool ok = v.size() == 16;
        glm::mat4 m(1.0F);
        for (std::size_t i = 0; ok && i < 16; ++i) {
            ok = JsonFields::read_float(v[i], m[static_cast<int>(i / 4)][static_cast<int>(i % 4)]);
        }
        if (!ok) return std::unexpected(core::Error{"transform: expected 16 finite numbers"});
        t.local = m;
        return {};
    }
    JsonFields f(v, "transform", strict);
    Trs trs = decompose(t.local);
    f.vec3("position", trs.position).vec3("rotation", trs.rotation_deg);
    if (const json* s = f.raw("scale")) {
        float uniform = 0.0F;
        if (JsonFields::read_float(*s, uniform)) {
            trs.scale = glm::vec3(uniform);
        } else if (!JsonFields::read_vec3(*s, trs.scale)) {
            f.fail("scale", "expected a number or [x, y, z]");
        }
    }
    glm::vec3 target(0.0F);
    const bool has_target = f.has("look_at");
    f.vec3("look_at", target);
    f.known({"world_position"});
    (void)f.raw("world_position"); // echoed back by encode; ignored on input
    if (has_target && f.ok()) {
        // Orient so local -Z (the engine's forward) faces the target point
        // (in the parent's space). Forward under YXZ with no roll is
        // (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch).
        const glm::vec3 d = target - trs.position;
        if (glm::length(d) < 1e-5F) {
            f.fail("look_at", "target coincides with the position");
        } else {
            const glm::vec3 dir = glm::normalize(d);
            trs.rotation_deg = glm::degrees(glm::vec3(std::asin(glm::clamp(dir.y, -1.0F, 1.0F)),
                                                      std::atan2(-dir.x, -dir.z), 0.0F));
        }
    }
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    if (glm::any(glm::lessThan(glm::abs(trs.scale), glm::vec3(1e-6F)))) {
        return std::unexpected(core::Error{"transform.scale: components must be non-zero"});
    }
    t.local = compose(trs);
    return {};
}

// ---- Colour helpers -----------------------------------------------------

[[nodiscard]] float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

void read_color(JsonFields& f, std::string_view key, glm::vec4& out) {
    if (!f.has(key)) return;
    const json* v = f.raw(key);
    glm::vec4 c = out;
    if (auto ok = decode_color(*v, c); !ok) {
        f.fail(key, ok.error().message);
        return;
    }
    out = c;
}

// ---- Merge helper --------------------------------------------------------

template <typename T, typename Decode>
[[nodiscard]] std::expected<void, core::Error> merge(entt::registry& r, entt::entity e,
                                                     const json& v, std::string_view name,
                                                     bool strict, Decode&& decode) {
    T component = r.all_of<T>(e) ? r.get<T>(e) : T{};
    JsonFields f(v, std::string(name), strict);
    decode(f, component);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    r.emplace_or_replace<T>(e, std::move(component));
    return {};
}

// Editing a physics component invalidates its body/character: resetting the
// handle makes the physics systems' orphan sweep drop the old one and build
// a new one from the edited data on the next tick.
void invalidate_physics(entt::registry& r, entt::entity e) {
    if (auto* rb = r.try_get<RigidBody>(e)) rb->body = physics::PhysicsWorld::invalid_body;
    if (auto* cc = r.try_get<CharacterController>(e)) {
        cc->character = physics::PhysicsWorld::invalid_body;
    }
}

} // namespace

std::expected<void, core::Error> decode_color(const json& v, glm::vec4& out) {
    constexpr const char* expected_msg =
        "expected \"#rrggbb\", \"#rrggbbaa\" (sRGB) or [r, g, b(, a)] in 0..1 (linear)";
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        const bool shape_ok = (s.size() == 7 || s.size() == 9) && s[0] == '#';
        std::array<float, 4> c{0.0F, 0.0F, 0.0F, 1.0F};
        bool ok = shape_ok;
        for (std::size_t i = 0; ok && i + 1 < s.size(); i += 2) {
            unsigned value = 0;
            for (std::size_t k = 1 + i; k < 3 + i && ok; ++k) {
                const char ch = s[k];
                const int digit = ch >= '0' && ch <= '9'   ? ch - '0'
                                  : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                  : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                           : -1;
                ok = digit >= 0;
                value = value * 16U + static_cast<unsigned>(digit < 0 ? 0 : digit);
            }
            c[i / 2] = static_cast<float>(value) / 255.0F;
        }
        if (!ok) return std::unexpected(core::Error{expected_msg});
        out = {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2]), c[3]};
        return {};
    }
    // Re-read through the checked array path.
    json wrapper = json::object();
    wrapper["c"] = v;
    JsonFields inner(wrapper, "");
    glm::vec4 c = out;
    inner.color4("c", c);
    if (auto ok = inner.finish(); !ok) return std::unexpected(core::Error{expected_msg});
    out = c;
    return {};
}

std::span<const ComponentInfo> component_infos() { return k_components; }

const ComponentInfo* find_component_info(std::string_view name) {
    for (const ComponentInfo& c : k_components) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

json encode_components(const entt::registry& r, entt::entity e, const EncodeOptions& options) {
    json out = json::object();
    if (const auto* t = r.try_get<Transform>(e)) {
        out["transform"] = encode_transform(*t, options.transform_as_matrix);
    }
    if (const auto* p = r.try_get<Primitive>(e)) {
        out["primitive"] = {{"shape", enum_name(k_primitive_shapes, p->shape)},
                            {"color", vec4_json(p->color)},
                            {"metallic", p->metallic},
                            {"roughness", p->roughness},
                            {"emissive", vec3_json(p->emissive)}};
    }
    if (const auto* l = r.try_get<PointLight>(e)) {
        out["point_light"] = {{"color", vec3_json(l->color)},
                              {"radius", l->radius},
                              {"intensity", l->intensity}};
    }
    if (const auto* l = r.try_get<DirectionalLight>(e)) {
        out["dir_light"] = {{"direction", vec3_json(l->direction)},
                            {"color", vec3_json(l->color)},
                            {"intensity", l->intensity}};
    }
    if (const auto* c = r.try_get<Camera>(e)) {
        out["camera"] = {{"fov_deg", glm::degrees(c->fov_y)},
                         {"near", c->near_z},
                         {"far", c->far_z},
                         {"active", c->is_active}};
    }
    if (const auto* c = r.try_get<Collider>(e)) {
        out["collider"] = {{"shape", enum_name(k_collider_shapes, c->shape)},
                           {"half_extents", vec3_json(c->half_extents)},
                           {"offset", vec3_json(c->offset)},
                           {"sensor", c->is_sensor}};
    }
    if (const auto* b = r.try_get<RigidBody>(e)) {
        out["rigidbody"] = {{"motion", enum_name(k_motions, b->motion)}, {"mass", b->mass}};
    }
    if (const auto* c = r.try_get<CharacterController>(e)) {
        out["character"] = {{"radius", c->radius},
                            {"half_height", c->half_height},
                            {"speed", c->speed}};
    }
    if (const auto* a = r.try_get<NavAgent>(e)) {
        out["nav_agent"] = {{"target", vec3_json(a->target)},
                            {"speed", a->speed},
                            {"ground_offset", a->ground_offset},
                            {"active", a->active}};
    }
    if (const auto* b = r.try_get<BehaviorTree>(e)) {
        out["behavior"] = {{"source", b->source}, {"enabled", b->enabled}};
    }
    if (const auto* p = r.try_get<PatrolRoute>(e)) {
        json pts = json::array();
        for (const glm::vec3& pt : p->points) pts.push_back(vec3_json(pt));
        out["patrol"] = {{"points", std::move(pts)}, {"loop", p->loop}};
    }
    if (const auto* t = r.try_get<Terrain>(e)) {
        out["terrain"] = {{"seed", t->params.seed},
                          {"resolution", t->params.resolution},
                          {"tile_size", t->params.tile_size_m},
                          {"height", t->params.height_m},
                          {"octaves", t->params.octaves},
                          {"warp", t->params.warp_strength},
                          {"droplets", t->params.erosion_droplets},
                          {"snowline", t->snowline_m},
                          {"streaming", t->streaming},
                          {"stream_radius", t->stream_radius}};
    }
    if (const auto* n = r.try_get<NetReplicated>(e)) out["net_replicated"] = {{"net_id", n->net_id}};
    if (const auto* a = r.try_get<Animator>(e)) {
        out["animator"] = {{"time", a->time}, {"speed", a->speed}, {"looping", a->looping}};
    }
    if (options.include_read_only) {
        if (const auto* m = r.try_get<MeshSource>(e)) {
            out["mesh"] = {{"mesh_key", m->mesh_key},
                           {"material_key", m->material_key},
                           {"gltf", m->gltf_path}};
        }
    }
    return out;
}

std::vector<std::string_view> component_names(const entt::registry& r, entt::entity e,
                                              bool include_read_only) {
    std::vector<std::string_view> out;
    if (!r.valid(e)) return out;
    const auto add = [&](bool present, std::string_view name) {
        if (present) out.push_back(name);
    };
    add(r.all_of<Transform>(e), "transform");
    add(r.all_of<Primitive>(e), "primitive");
    add(r.all_of<PointLight>(e), "point_light");
    add(r.all_of<DirectionalLight>(e), "dir_light");
    add(r.all_of<Camera>(e), "camera");
    add(r.all_of<Collider>(e), "collider");
    add(r.all_of<RigidBody>(e), "rigidbody");
    add(r.all_of<CharacterController>(e), "character");
    add(r.all_of<NavAgent>(e), "nav_agent");
    add(r.all_of<BehaviorTree>(e), "behavior");
    add(r.all_of<PatrolRoute>(e), "patrol");
    add(r.all_of<Terrain>(e), "terrain");
    add(r.all_of<NetReplicated>(e), "net_replicated");
    add(r.all_of<Animator>(e), "animator");
    add(include_read_only && r.all_of<MeshSource>(e), "mesh");
    return out;
}

std::expected<void, core::Error> apply_component(entt::registry& r, entt::entity e,
                                                 std::string_view name, const json& v,
                                                 bool strict) {
    if (!r.valid(e)) return std::unexpected(core::Error{"entity does not exist"});
    const ComponentInfo* info = find_component_info(name);
    if (info == nullptr) {
        std::string known;
        for (const ComponentInfo& c : k_components) {
            if (c.writable) known += (known.empty() ? "" : ", ") + std::string(c.name);
        }
        return std::unexpected(
            core::Error{"unknown component '" + std::string(name) + "' (known: " + known + ")"});
    }
    if (!info->writable) {
        return std::unexpected(core::Error{"component '" + std::string(name) + "' is read-only"});
    }

    if (name == "transform") {
        Transform t = r.all_of<Transform>(e) ? r.get<Transform>(e) : Transform{};
        if (auto ok = decode_transform(t, v, strict); !ok) return ok;
        r.emplace_or_replace<Transform>(e, t);
        return {};
    }
    if (name == "primitive") {
        auto ok = merge<Primitive>(r, e, v, name, strict, [](JsonFields& f, Primitive& p) {
            f.enumeration("shape", p.shape, k_primitive_shapes);
            read_color(f, "color", p.color);
            f.number("metallic", p.metallic, 0.0F, 1.0F).number("roughness", p.roughness, 0.0F, 1.0F);
            f.color3("emissive", p.emissive);
            p.resolved = 0; // rebuild the MeshRenderer
        });
        if (ok) r.remove<MeshSource>(e); // the primitive is now the mesh's provenance
        return ok;
    }
    if (name == "point_light") {
        return merge<PointLight>(r, e, v, name, strict, [](JsonFields& f, PointLight& l) {
            f.color3("color", l.color).number("radius", l.radius, 0.01F).number("intensity", l.intensity, 0.0F);
        });
    }
    if (name == "dir_light") {
        return merge<DirectionalLight>(r, e, v, name, strict, [](JsonFields& f, DirectionalLight& l) {
            f.vec3("direction", l.direction).color3("color", l.color).number("intensity", l.intensity, 0.0F);
            if (glm::length(l.direction) < 1e-6F) {
                f.fail("direction", "must be non-zero");
            } else {
                l.direction = glm::normalize(l.direction);
            }
        });
    }
    if (name == "camera") {
        return merge<Camera>(r, e, v, name, strict, [](JsonFields& f, Camera& c) {
            float fov_deg = glm::degrees(c.fov_y);
            f.number("fov_deg", fov_deg, 1.0F, 179.0F);
            if (f.has("fov_y")) { // legacy scene files: radians
                float fov_rad = c.fov_y;
                f.number("fov_y", fov_rad, glm::radians(1.0F), glm::radians(179.0F));
                fov_deg = glm::degrees(fov_rad);
            }
            c.fov_y = glm::radians(fov_deg);
            f.number("near", c.near_z, 1e-4F).number("far", c.far_z, 1e-3F).boolean("active", c.is_active);
            if (c.far_z <= c.near_z) f.fail("far", "must be greater than near");
        });
    }
    if (name == "collider") {
        auto ok = merge<Collider>(r, e, v, name, strict, [](JsonFields& f, Collider& c) {
            f.enumeration("shape", c.shape, k_collider_shapes);
            f.vec3("half_extents", c.half_extents).vec3("offset", c.offset).boolean("sensor", c.is_sensor);
            if (glm::any(glm::lessThanEqual(c.half_extents, glm::vec3(0.0F)))) {
                f.fail("half_extents", "components must be > 0");
            }
        });
        if (ok) invalidate_physics(r, e);
        return ok;
    }
    if (name == "rigidbody") {
        auto ok = merge<RigidBody>(r, e, v, name, strict, [](JsonFields& f, RigidBody& b) {
            f.enumeration("motion", b.motion, k_motions).number("mass", b.mass, 0.0F);
        });
        if (ok) invalidate_physics(r, e);
        return ok;
    }
    if (name == "character") {
        auto ok = merge<CharacterController>(r, e, v, name, strict, [](JsonFields& f, CharacterController& c) {
            f.number("radius", c.radius, 0.01F).number("half_height", c.half_height, 0.0F);
            f.number("speed", c.speed, 0.0F);
        });
        if (ok) invalidate_physics(r, e);
        return ok;
    }
    if (name == "nav_agent") {
        return merge<NavAgent>(r, e, v, name, strict, [](JsonFields& f, NavAgent& a) {
            const glm::vec3 old_target = a.target;
            f.vec3("target", a.target).number("speed", a.speed, 0.0F);
            f.number("ground_offset", a.ground_offset).boolean("active", a.active);
            if (a.target != old_target) a.arrived = false; // the system repaths
        });
    }
    if (name == "behavior") {
        return merge<BehaviorTree>(r, e, v, name, strict, [](JsonFields& f, BehaviorTree& b) {
            if (const json* src = f.raw("source")) {
                if (src->is_string()) {
                    b.source = src->get<std::string>();
                } else if (src->is_object()) { // a tree given inline as JSON
                    b.source = src->dump(-1, ' ', false, json::error_handler_t::replace);
                } else {
                    f.fail("source", "expected a behaviour tree (object) or its JSON text");
                }
                b.compiled.reset(); // recompile lazily
                b.node_state.clear();
                b.parse_failed = false;
            }
            f.boolean("enabled", b.enabled);
        });
    }
    if (name == "patrol") {
        return merge<PatrolRoute>(r, e, v, name, strict, [](JsonFields& f, PatrolRoute& p) {
            if (f.has("points")) p.next = 0;
            f.vec3_list("points", p.points).boolean("loop", p.loop);
        });
    }
    if (name == "terrain") {
        return merge<Terrain>(r, e, v, name, strict, [](JsonFields& f, Terrain& t) {
            terrain::TerrainParams& p = t.params;
            f.integer("seed", p.seed, 0, 0xFFFFFFFFLL);
            f.integer("resolution", p.resolution, 17, 8193);
            f.number("tile_size", p.tile_size_m, 1.0F, 100000.0F);
            f.number("height", p.height_m, 0.0F, 100000.0F);
            f.integer("octaves", p.octaves, 1, 16);
            f.number("warp", p.warp_strength, 0.0F, 4.0F);
            f.integer("droplets", p.erosion_droplets, 0, 50'000'000);
            f.number("snowline", t.snowline_m);
            f.boolean("streaming", t.streaming);
            f.integer("stream_radius", t.stream_radius, 0, 8);
            t.regenerate = true; // rebuild with the new parameters
        });
    }
    if (name == "net_replicated") {
        return merge<NetReplicated>(r, e, v, name, strict, [](JsonFields& f, NetReplicated& n) {
            f.integer("net_id", n.net_id, 0, 0xFFFFFFFFLL);
        });
    }
    if (name == "animator") {
        if (!r.all_of<Animator>(e)) {
            return std::unexpected(core::Error{
                "animator: the entity has no animation (animators come from glTF files)"});
        }
        return merge<Animator>(r, e, v, name, strict, [](JsonFields& f, Animator& a) {
            f.number("time", a.time, 0.0F).number("speed", a.speed, -100.0F, 100.0F);
            f.boolean("looping", a.looping);
        });
    }
    return std::unexpected(core::Error{"component '" + std::string(name) + "' has no decoder"});
}

bool remove_component(entt::registry& r, entt::entity e, std::string_view name) {
    if (!r.valid(e)) return false;
    if (name == "primitive") {
        const bool had = r.remove<Primitive>(e) > 0;
        if (had) r.remove<MeshRenderer>(e);
        return had;
    }
    if (name == "mesh") {
        const bool had = r.remove<MeshSource>(e) > 0;
        r.remove<MeshRenderer, SkinnedMesh, Animator>(e);
        return had;
    }
    if (name == "point_light") return r.remove<PointLight>(e) > 0;
    if (name == "dir_light") return r.remove<DirectionalLight>(e) > 0;
    if (name == "camera") return r.remove<Camera>(e) > 0;
    if (name == "collider") return r.remove<Collider>(e) > 0;
    if (name == "rigidbody") return r.remove<RigidBody>(e) > 0;
    if (name == "character") return r.remove<CharacterController>(e) > 0;
    if (name == "nav_agent") return r.remove<NavAgent>(e) > 0;
    if (name == "behavior") return r.remove<BehaviorTree>(e) > 0;
    if (name == "patrol") return r.remove<PatrolRoute>(e) > 0;
    if (name == "terrain") return r.remove<Terrain>(e) > 0;
    if (name == "net_replicated") return r.remove<NetReplicated>(e) > 0;
    if (name == "animator") return r.remove<Animator>(e) > 0;
    return false; // transform (not removable) or unknown
}

} // namespace engine::scene
