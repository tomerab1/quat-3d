// Component JSON codec — see include/engine/scene/component_json.hpp.

#define GLM_ENABLE_EXPERIMENTAL
#include "engine/scene/component_json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/script_component.hpp"

namespace engine::scene {

namespace {

using json = nlohmann::json;
using core::EnumName;
using core::JsonFields;

constexpr std::array k_components = {
    ComponentInfo{"transform", "Local position / rotation (Euler degrees, YXZ) / scale", false, true},
    ComponentInfo{"primitive", "Procedural shape (cube, sphere, plane, cylinder, cone, capsule) with a PBR colour", true, true},
    ComponentInfo{"material", "Per-entity overrides of the drawn material's PBR factors (null clears one)", true, true},
    ComponentInfo{"instanced_mesh", "Many placements of one model (glTF or primitive shape), drawn instanced", true, true},
    ComponentInfo{"scatter", "Seeded procedural placement of the entity's instanced mesh on the terrain", true, true},
    ComponentInfo{"point_light", "Point light at the entity position", true, true},
    ComponentInfo{"dir_light", "Directional light (the sun)", true, true},
    ComponentInfo{"camera", "Perspective camera", true, true},
    ComponentInfo{"collider", "Physics collision shape (box, sphere, capsule)", true, true},
    ComponentInfo{"rigidbody", "Physics body: static, kinematic or dynamic", true, true},
    ComponentInfo{"character", "Capsule character controller", true, true},
    ComponentInfo{"nav_agent", "Navmesh path-following agent", true, true},
    ComponentInfo{"behavior", "Behaviour tree (JSON source)", true, true},
    ComponentInfo{"patrol", "Patrol route the nav agent walks", true, true},
    ComponentInfo{"script", "Lua gameplay script (assets/scripts/) with property overrides", true, true},
    ComponentInfo{"terrain", "Procedural terrain tile / streamed world", true, true},
    ComponentInfo{"grass", "GPU-generated, wind-animated grass around the camera", true, true},
    ComponentInfo{"net_replicated", "Replicate this entity's transform to network clients", true, true},
    ComponentInfo{"audio_source", "Sound emitter: wav/flac/mp3 clip on a bus, optionally 3D", true, true},
    ComponentInfo{"audio_listener", "The audio listener follows this entity (else the camera)", true, true},
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

constexpr EnumName<ScatterArea> k_scatter_areas[] = {{"rect", ScatterArea::rect},
                                                     {"circle", ScatterArea::circle}};
constexpr EnumName<ScatterPattern> k_scatter_patterns[] = {
    {"poisson", ScatterPattern::poisson}, {"jittered_grid", ScatterPattern::jittered_grid}};
constexpr std::size_t k_max_asset_path = 4096;

constexpr EnumName<audio::AudioBus> k_audio_buses[] = {
    {"master", audio::AudioBus::master}, {"music", audio::AudioBus::music},
    {"sfx", audio::AudioBus::sfx},       {"ui", audio::AudioBus::ui},
    {"ambience", audio::AudioBus::ambience}};
constexpr EnumName<terrain::ErosionModel> k_erosion_models[] = {
    {"droplets", terrain::ErosionModel::droplets}, {"grid", terrain::ErosionModel::grid}};
constexpr EnumName<AudioCommand> k_audio_commands[] = {
    {"none", AudioCommand::none}, {"play", AudioCommand::play}, {"stop", AudioCommand::stop},
    {"pause", AudioCommand::pause}, {"resume", AudioCommand::resume}};
constexpr std::size_t k_max_clip_path = 4096;

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

// ---- Instanced meshes ---------------------------------------------------

constexpr std::size_t k_max_instances = 100'000;

[[nodiscard]] json encode_instance(const InstanceTransform& t) {
    float yaw = 0.0F;
    float pitch = 0.0F;
    float roll = 0.0F;
    glm::extractEulerAngleYXZ(glm::mat4_cast(t.rotation), yaw, pitch, roll);
    json out = {{"position", vec3_json(t.position)},
                {"rotation", vec3_json(glm::degrees(glm::vec3(pitch, yaw, roll)))}};
    if (t.scale.x == t.scale.y && t.scale.x == t.scale.z) {
        out["scale"] = t.scale.x;
    } else {
        out["scale"] = vec3_json(t.scale);
    }
    return out;
}

// One placement: {position, rotation (Euler degrees, YXZ), scale (number or
// [x, y, z])} with identity defaults, or a bare [x, y, z] position.
[[nodiscard]] std::expected<InstanceTransform, core::Error>
decode_instance(const json& v, const std::string& where, bool strict) {
    InstanceTransform t;
    if (v.is_array()) {
        if (!JsonFields::read_vec3(v, t.position)) {
            return std::unexpected(core::Error{where + ": expected [x, y, z] or an object"});
        }
        return t;
    }
    JsonFields f(v, where, strict);
    glm::vec3 rotation_deg(0.0F);
    f.vec3("position", t.position).vec3("rotation", rotation_deg);
    f.known({"scale"});
    if (const json* sc = f.raw("scale")) {
        float uniform = 0.0F;
        if (JsonFields::read_float(*sc, uniform)) {
            t.scale = glm::vec3(uniform);
        } else if (!JsonFields::read_vec3(*sc, t.scale)) {
            f.fail("scale", "expected a number or [x, y, z]");
        }
    }
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    if (glm::any(glm::lessThan(glm::abs(t.scale), glm::vec3(1e-6F)))) {
        return std::unexpected(core::Error{where + ".scale: components must be non-zero"});
    }
    const glm::vec3 r = glm::radians(rotation_deg);
    t.rotation = glm::normalize(glm::quat_cast(glm::eulerAngleYXZ(r.y, r.x, r.z)));
    return t;
}

// Asset paths in components are relative to the project root and may not
// climb out of it (remote clients write them).
void check_project_path(JsonFields& f, std::string_view key, const std::string& path) {
    if (path.empty()) return;
    const std::filesystem::path p(path);
    bool climbs = false;
    for (const auto& part : p) climbs = climbs || part == "..";
    if (path.find('\0') != std::string::npos || p.is_absolute() || p.has_root_name() ||
        p.has_root_directory() || climbs) {
        f.fail(key, "expected a path relative to the project root (no '..')");
    }
}

// Reads a [min, max] pair within [lo, hi] (min <= max).
void read_range(JsonFields& f, std::string_view key, glm::vec2& out, float lo, float hi) {
    f.known({key});
    const json* v = f.raw(key);
    if (v == nullptr || v->is_null()) return;
    glm::vec2 r{};
    const bool ok = v->is_array() && v->size() == 2 && JsonFields::read_float((*v)[0], r.x) &&
                    JsonFields::read_float((*v)[1], r.y) && r.x >= lo && r.y <= hi && r.x <= r.y;
    if (!ok) {
        const auto trim = [](float x) {
            std::string t = std::to_string(x);
            while (t.size() > 1 && t.back() == '0') t.pop_back();
            if (!t.empty() && t.back() == '.') t.pop_back();
            return t;
        };
        f.fail(key, "expected [min, max] with " + trim(lo) + " <= min <= max <= " + trim(hi));
        return;
    }
    out = r;
}

// The shape + flat material fields shared by `primitive` and `instanced_mesh`.
void read_primitive_fields(JsonFields& f, Primitive& p) {
    f.enumeration("shape", p.shape, k_primitive_shapes);
    f.known({"color"});
    read_color(f, "color", p.color);
    f.number("metallic", p.metallic, 0.0F, 1.0F).number("roughness", p.roughness, 0.0F, 1.0F);
    f.color3("emissive", p.emissive);
}

[[nodiscard]] json primitive_fields_json(const Primitive& p) {
    return {{"shape", enum_name(k_primitive_shapes, p.shape)},
            {"color", vec4_json(p.color)},
            {"metallic", p.metallic},
            {"roughness", p.roughness},
            {"emissive", vec3_json(p.emissive)}};
}

// ---- Grass ---------------------------------------------------------------

[[nodiscard]] json range_json(const glm::vec2& r) { return json::array({r.x, r.y}); }

[[nodiscard]] json encode_grass(const renderer::GrassSettings& g) {
    return {{"seed", g.seed},
            {"density", g.density},
            {"radius", g.radius},
            {"blade_height", range_json(g.height_range)},
            {"blade_width", range_json(g.width_range)},
            {"base_color", vec3_json(g.base_color)},
            {"tip_color", vec3_json(g.tip_color)},
            {"wind_strength", g.wind_strength},
            {"wind_direction", range_json(g.wind_direction)},
            {"wind_frequency", g.wind_frequency},
            {"slope", range_json(g.slope_deg)},
            {"height", range_json(g.height_m)},
            {"terrain_only", g.terrain_only},
            {"ground_y", g.ground_y},
            {"lod_start", g.lod_start},
            {"far_density", g.far_density}};
}

// "#rrggbb" (sRGB) or linear [r, g, b] in 0..1.
void read_color3(JsonFields& f, std::string_view key, glm::vec3& out) {
    f.known({key});
    glm::vec4 c(out, 1.0F);
    read_color(f, key, c);
    out = glm::vec3(c);
}

void decode_grass(JsonFields& f, renderer::GrassSettings& g) {
    f.integer("seed", g.seed, 0, 0xFFFFFFFFLL);
    f.number("density", g.density, 0.0F, 1000.0F).number("radius", g.radius, 0.0F, 500.0F);
    read_range(f, "blade_height", g.height_range, 0.01F, 5.0F);
    read_range(f, "blade_width", g.width_range, 0.001F, 0.5F);
    read_color3(f, "base_color", g.base_color);
    read_color3(f, "tip_color", g.tip_color);
    f.number("wind_strength", g.wind_strength, 0.0F, 4.0F);
    f.number("wind_frequency", g.wind_frequency, 0.0F, 10.0F);
    f.known({"wind_direction"});
    if (const json* w = f.raw("wind_direction"); w != nullptr && !w->is_null()) {
        glm::vec2 d{};
        if (w->is_array() && w->size() == 2 && JsonFields::read_float((*w)[0], d.x) &&
            JsonFields::read_float((*w)[1], d.y) && glm::length(d) > 1e-6F) {
            const float len = glm::length(d); // already-unit input round-trips exactly
            g.wind_direction = std::abs(len - 1.0F) < 1e-6F ? d : d / len;
        } else {
            f.fail("wind_direction", "expected a non-zero [x, z] direction");
        }
    }
    read_range(f, "slope", g.slope_deg, 0.0F, 90.0F);
    read_range(f, "height", g.height_m, -100000.0F, 100000.0F);
    f.boolean("terrain_only", g.terrain_only);
    f.number("ground_y", g.ground_y, -100000.0F, 100000.0F);
    f.number("lod_start", g.lod_start, 0.0F, 1.0F).number("far_density", g.far_density, 0.0F, 1.0F);
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

namespace {

// Looks a component up only when the caller asked for it by name.
struct Picker {
    const entt::registry&        r;
    entt::entity                 e;
    std::span<const std::string> only;

    template <typename C>
    [[nodiscard]] const C* get(std::string_view name) const {
        if (!only.empty() && std::ranges::find(only, name) == only.end()) return nullptr;
        return r.try_get<C>(e);
    }
};

} // namespace

json encode_components(const entt::registry& r, entt::entity e, const EncodeOptions& options) {
    json out = json::object();
    const Picker pick{r, e, options.only};
    if (const auto* t = pick.get<Transform>("transform")) {
        out["transform"] = encode_transform(*t, options.transform_as_matrix);
    }
    if (const auto* p = pick.get<Primitive>("primitive")) {
        out["primitive"] = primitive_fields_json(*p);
    }
    if (const auto* m = pick.get<MaterialOverride>("material"); m != nullptr && m->mask != 0) {
        out["material"] = encode_material_override(*m);
    }
    if (const auto* m = pick.get<InstancedMesh>("instanced_mesh")) {
        json j = primitive_fields_json(m->primitive);
        j["gltf"] = m->gltf_path;
        j["node"] = m->gltf_node;
        j["cast_shadows"] = m->cast_shadows;
        j["instance_count"] = m->instances.size();
        // A scatter regenerates the placements: they are derived data, not
        // part of the entity's description (and can number in the tens of
        // thousands).
        if (!r.all_of<Scatter>(e)) {
            json list = json::array();
            for (const InstanceTransform& t : m->instances) list.push_back(encode_instance(t));
            j["instances"] = std::move(list);
        }
        out["instanced_mesh"] = std::move(j);
    }
    if (const auto* sc = pick.get<Scatter>("scatter")) {
        out["scatter"] = {{"seed", sc->seed},
                          {"area", enum_name(k_scatter_areas, sc->area)},
                          {"size", json::array({sc->size.x, sc->size.y})},
                          {"radius", sc->radius},
                          {"density", sc->density},
                          {"min_spacing", sc->min_spacing},
                          {"pattern", enum_name(k_scatter_patterns, sc->pattern)},
                          {"scale", json::array({sc->scale_range.x, sc->scale_range.y})},
                          {"random_yaw", sc->random_yaw},
                          {"align_to_normal", sc->align_to_normal},
                          {"base_offset", sc->base_offset},
                          {"slope", json::array({sc->slope_deg.x, sc->slope_deg.y})},
                          {"height", json::array({sc->height_m.x, sc->height_m.y})},
                          {"max_instances", sc->max_instances}};
    }
    if (const auto* l = pick.get<PointLight>("point_light")) {
        out["point_light"] = {{"color", vec3_json(l->color)},
                              {"radius", l->radius},
                              {"intensity", l->intensity}};
    }
    if (const auto* l = pick.get<DirectionalLight>("dir_light")) {
        out["dir_light"] = {{"direction", vec3_json(l->direction)},
                            {"color", vec3_json(l->color)},
                            {"intensity", l->intensity}};
    }
    if (const auto* c = pick.get<Camera>("camera")) {
        out["camera"] = {{"fov_deg", glm::degrees(c->fov_y)},
                         {"near", c->near_z},
                         {"far", c->far_z},
                         {"active", c->is_active}};
    }
    if (const auto* c = pick.get<Collider>("collider")) {
        out["collider"] = {{"shape", enum_name(k_collider_shapes, c->shape)},
                           {"half_extents", vec3_json(c->half_extents)},
                           {"offset", vec3_json(c->offset)},
                           {"sensor", c->is_sensor}};
    }
    if (const auto* b = pick.get<RigidBody>("rigidbody")) {
        out["rigidbody"] = {{"motion", enum_name(k_motions, b->motion)}, {"mass", b->mass}};
    }
    if (const auto* c = pick.get<CharacterController>("character")) {
        out["character"] = {{"radius", c->radius},
                            {"half_height", c->half_height},
                            {"speed", c->speed}};
    }
    if (const auto* a = pick.get<NavAgent>("nav_agent")) {
        out["nav_agent"] = {{"target", vec3_json(a->target)},
                            {"speed", a->speed},
                            {"ground_offset", a->ground_offset},
                            {"active", a->active},
                            {"paused", a->paused}};
    }
    if (const auto* b = pick.get<BehaviorTree>("behavior")) {
        out["behavior"] = {{"source", b->source}, {"enabled", b->enabled}};
    }
    if (const auto* sc = pick.get<Script>("script")) out["script"] = encode_script(*sc);
    if (const auto* p = pick.get<PatrolRoute>("patrol")) {
        json pts = json::array();
        for (const glm::vec3& pt : p->points) pts.push_back(vec3_json(pt));
        out["patrol"] = {{"points", std::move(pts)}, {"loop", p->loop}};
    }
    if (const auto* t = pick.get<Terrain>("terrain")) {
        out["terrain"] = {{"seed", t->params.seed},
                          {"resolution", t->params.resolution},
                          {"tile_size", t->params.tile_size_m},
                          {"height", t->params.height_m},
                          {"octaves", t->params.octaves},
                          {"warp", t->params.warp_strength},
                          {"erosion", enum_name(k_erosion_models, t->params.erosion_model)},
                          {"droplets", t->params.erosion_droplets},
                          {"erosion_steps", t->params.grid.iterations},
                          {"snowline", t->snowline_m},
                          {"streaming", t->streaming},
                          {"stream_radius", t->stream_radius},
                          {"gpu", t->gpu_generate}};
    }
    if (const auto* g = pick.get<Grass>("grass")) out["grass"] = encode_grass(g->settings);
    if (const auto* n = pick.get<NetReplicated>("net_replicated")) {
        out["net_replicated"] = {{"net_id", n->net_id}};
    }
    if (const auto* a = pick.get<AudioSource>("audio_source")) {
        out["audio_source"] = {{"clip", a->clip},
                               {"bus", enum_name(k_audio_buses, a->bus)},
                               {"volume", a->volume},
                               {"pitch", a->pitch},
                               {"loop", a->loop},
                               {"play_on_start", a->play_on_start},
                               {"spatial", a->spatial},
                               {"min_distance", a->min_distance},
                               {"max_distance", a->max_distance},
                               {"rolloff", a->rolloff},
                               {"doppler", a->doppler}};
    }
    if (const auto* l = pick.get<AudioListener>("audio_listener")) {
        out["audio_listener"] = {{"active", l->active}};
    }
    if (const auto* a = pick.get<Animator>("animator")) {
        out["animator"] = {{"time", a->time}, {"speed", a->speed}, {"looping", a->looping}};
    }
    if (options.include_read_only) {
        if (const auto* m = pick.get<MeshSource>("mesh")) {
            out["mesh"] = {{"mesh_key", m->mesh_key},
                           {"material_key", m->material_key},
                           {"gltf", m->gltf_path}};
            if (m->gltf_node >= 0) out["mesh"]["node"] = m->gltf_node;
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
    const auto* material = r.try_get<MaterialOverride>(e);
    add(material != nullptr && material->mask != 0, "material");
    add(r.all_of<InstancedMesh>(e), "instanced_mesh");
    add(r.all_of<Scatter>(e), "scatter");
    add(r.all_of<PointLight>(e), "point_light");
    add(r.all_of<DirectionalLight>(e), "dir_light");
    add(r.all_of<Camera>(e), "camera");
    add(r.all_of<Collider>(e), "collider");
    add(r.all_of<RigidBody>(e), "rigidbody");
    add(r.all_of<CharacterController>(e), "character");
    add(r.all_of<NavAgent>(e), "nav_agent");
    add(r.all_of<BehaviorTree>(e), "behavior");
    add(r.all_of<PatrolRoute>(e), "patrol");
    add(r.all_of<Script>(e), "script");
    add(r.all_of<Terrain>(e), "terrain");
    add(r.all_of<Grass>(e), "grass");
    add(r.all_of<NetReplicated>(e), "net_replicated");
    add(r.all_of<AudioSource>(e), "audio_source");
    add(r.all_of<AudioListener>(e), "audio_listener");
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
            read_primitive_fields(f, p);
            p.resolved = 0; // rebuild the MeshRenderer
        });
        if (ok) r.remove<MeshSource>(e); // the primitive is now the mesh's provenance
        return ok;
    }
    if (name == "material") {
        MaterialOverride o = r.all_of<MaterialOverride>(e) ? r.get<MaterialOverride>(e)
                                                           : MaterialOverride{};
        if (auto ok = decode_material_override(v, o, strict); !ok) return ok;
        if (o.mask == 0) {
            r.remove<MaterialOverride>(e);
        } else {
            r.emplace_or_replace<MaterialOverride>(e, std::move(o));
        }
        return {};
    }
    if (name == "instanced_mesh") {
        const bool scattered = r.all_of<Scatter>(e);
        return merge<InstancedMesh>(r, e, v, name, strict, [&](JsonFields& f, InstancedMesh& m) {
            read_primitive_fields(f, m.primitive);
            f.string("gltf", m.gltf_path, k_max_asset_path).string("node", m.gltf_node, 1024);
            check_project_path(f, "gltf", m.gltf_path);
            f.boolean("cast_shadows", m.cast_shadows);
            f.known({"instance_count", "instances"});
            (void)f.raw("instance_count"); // echoed back by encode; ignored on input
            const json* list = f.raw("instances");
            if (list == nullptr || list->is_null()) return;
            if (scattered) {
                f.fail("instances", "generated by the entity's scatter (remove the scatter to "
                                    "place instances by hand)");
                return;
            }
            if (!list->is_array() || list->size() > k_max_instances) {
                f.fail("instances", "expected an array of at most " +
                                        std::to_string(k_max_instances) + " placements");
                return;
            }
            std::vector<InstanceTransform> placed;
            placed.reserve(list->size());
            for (std::size_t i = 0; i < list->size(); ++i) {
                auto t = decode_instance((*list)[i],
                                         "instanced_mesh.instances[" + std::to_string(i) + "]",
                                         strict);
                if (!t) {
                    f.fail("instances", t.error().message);
                    return;
                }
                placed.push_back(*t);
            }
            m.instances = std::move(placed);
        });
    }
    if (name == "scatter") {
        return merge<Scatter>(r, e, v, name, strict, [](JsonFields& f, Scatter& s) {
            f.integer("seed", s.seed, 0, 0xFFFFFFFFLL);
            f.enumeration("area", s.area, k_scatter_areas);
            f.known({"size"});
            if (const json* sz = f.raw("size"); sz != nullptr && !sz->is_null()) {
                glm::vec2 size{};
                if (sz->is_array() && sz->size() == 2 && JsonFields::read_float((*sz)[0], size.x) &&
                    JsonFields::read_float((*sz)[1], size.y) && size.x >= 0.0F && size.y >= 0.0F &&
                    size.x <= 20000.0F && size.y <= 20000.0F) {
                    s.size = size;
                } else {
                    f.fail("size", "expected [x, z] in metres, each in [0, 20000]");
                }
            }
            f.number("radius", s.radius, 0.0F, 10000.0F);
            f.number("density", s.density, 0.0F, 100.0F);
            f.number("min_spacing", s.min_spacing, 0.0F, 1000.0F);
            f.enumeration("pattern", s.pattern, k_scatter_patterns);
            read_range(f, "scale", s.scale_range, 0.001F, 1000.0F);
            f.boolean("random_yaw", s.random_yaw);
            f.number("align_to_normal", s.align_to_normal, 0.0F, 1.0F);
            f.number("base_offset", s.base_offset, -1000.0F, 1000.0F);
            read_range(f, "slope", s.slope_deg, 0.0F, 90.0F);
            read_range(f, "height", s.height_m, -100000.0F, 100000.0F);
            f.integer("max_instances", s.max_instances, 0, static_cast<std::int64_t>(k_max_instances));
        });
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
            f.boolean("paused", a.paused);
            if (a.target != old_target) { // a new walk: the system repaths
                a.commanded = true;          // ...and a route resumes after it
                a.arrived = false;
                a.failed = false;
                a.plan_failures = 0;
                a.retry_s = 0.0F;
                a.path.clear();
            }
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
                b.last_result.clear();
                b.parse_failed = false;
            }
            f.boolean("enabled", b.enabled);
        });
    }
    if (name == "patrol") {
        return merge<PatrolRoute>(r, e, v, name, strict, [](JsonFields& f, PatrolRoute& p) {
            if (f.has("points")) {
                p.next = 0;
                p.finished = false;
            }
            f.vec3_list("points", p.points).boolean("loop", p.loop);
        });
    }
    if (name == "script") {
        const bool adding = !r.all_of<Script>(e);
        return merge<Script>(r, e, v, name, strict, [adding](JsonFields& f, Script& sc) {
            if (adding && !f.has("path")) {
                f.fail("path", "required when adding a script (relative to assets/scripts/)");
            }
            decode_script_fields(f, sc);
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
            f.enumeration("erosion", p.erosion_model, k_erosion_models);
            f.integer("droplets", p.erosion_droplets, 0, 50'000'000);
            f.integer("erosion_steps", p.grid.iterations, 0, terrain::max_grid_erosion_steps);
            f.number("snowline", t.snowline_m);
            f.boolean("streaming", t.streaming);
            f.integer("stream_radius", t.stream_radius, 0, 8);
            f.boolean("gpu", t.gpu_generate);
            // Grid erosion's cost grows with resolution² x steps and has no
            // droplet-style cap on its own: bound it (audit D3).
            if (!terrain::grid_erosion_within_limits(p)) {
                f.fail(f.has("erosion_steps") ? "erosion_steps" : "resolution",
                       "grid erosion is limited to resolution " +
                           std::to_string(terrain::max_grid_erosion_resolution) +
                           " and resolution^2 x erosion_steps <= " +
                           std::to_string(terrain::max_grid_erosion_texel_steps) +
                           " (e.g. 1025 at 5000 steps)");
            }
            t.regenerate = true; // rebuild with the new parameters
        });
    }
    if (name == "grass") {
        return merge<Grass>(r, e, v, name, strict,
                            [](JsonFields& f, Grass& g) { decode_grass(f, g.settings); });
    }
    if (name == "net_replicated") {
        return merge<NetReplicated>(r, e, v, name, strict, [](JsonFields& f, NetReplicated& n) {
            f.integer("net_id", n.net_id, 0, 0xFFFFFFFFLL);
        });
    }
    if (name == "audio_source") {
        // `command` is write-only: the audio system consumes it (play, stop,
        // pause, resume) — how remote tools trigger a sound.
        return merge<AudioSource>(r, e, v, name, strict, [](JsonFields& f, AudioSource& a) {
            f.string("clip", a.clip, k_max_clip_path).enumeration("bus", a.bus, k_audio_buses);
            if (a.clip.find('\0') != std::string::npos) f.fail("clip", "must not contain NUL");
            f.number("volume", a.volume, 0.0F, 16.0F).number("pitch", a.pitch, 0.01F, 16.0F);
            f.boolean("loop", a.loop).boolean("play_on_start", a.play_on_start);
            f.boolean("spatial", a.spatial);
            f.number("min_distance", a.min_distance, 0.01F, 1.0e6F);
            f.number("max_distance", a.max_distance, 0.01F, 1.0e6F);
            f.number("rolloff", a.rolloff, 0.0F, 100.0F).number("doppler", a.doppler, 0.0F, 10.0F);
            f.enumeration("command", a.command, k_audio_commands);
            if (a.max_distance < a.min_distance) f.fail("max_distance", "must be >= min_distance");
        });
    }
    if (name == "audio_listener") {
        return merge<AudioListener>(r, e, v, name, strict,
                                    [](JsonFields& f, AudioListener& l) { f.boolean("active", l.active); });
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
    if (name == "instanced_mesh") {
        // A scatter without a mesh would re-add a default one: it goes too.
        const bool had = r.remove<InstancedMesh>(e) > 0;
        if (had) r.remove<InstancedMeshParts, Scatter>(e);
        return had;
    }
    if (name == "material") return r.remove<MaterialOverride>(e) > 0;
    if (name == "scatter") return r.remove<Scatter>(e) > 0; // keeps the placements (baked)
    if (name == "point_light") return r.remove<PointLight>(e) > 0;
    if (name == "dir_light") return r.remove<DirectionalLight>(e) > 0;
    if (name == "camera") return r.remove<Camera>(e) > 0;
    if (name == "collider") return r.remove<Collider>(e) > 0;
    if (name == "rigidbody") return r.remove<RigidBody>(e) > 0;
    if (name == "character") return r.remove<CharacterController>(e) > 0;
    if (name == "nav_agent") return r.remove<NavAgent>(e) > 0;
    if (name == "behavior") return r.remove<BehaviorTree>(e) > 0;
    if (name == "patrol") return r.remove<PatrolRoute>(e) > 0;
    if (name == "script") return r.remove<Script>(e) > 0;
    if (name == "terrain") return r.remove<Terrain>(e) > 0;
    if (name == "grass") return r.remove<Grass>(e) > 0;
    if (name == "net_replicated") return r.remove<NetReplicated>(e) > 0;
    if (name == "audio_source") return r.remove<AudioSource>(e) > 0;
    if (name == "audio_listener") return r.remove<AudioListener>(e) > 0;
    if (name == "animator") return r.remove<Animator>(e) > 0;
    return false; // transform (not removable) or unknown
}

} // namespace engine::scene
