// Helpers shared by the per-component codec files (src/scene/codec/*.cpp):
// colours, primitive fields, project paths, ranges, physics invalidation.
// See component_json_internal.hpp.

#include "component_json_internal.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

#include "engine/physics/physics_world.hpp"
#include "engine/rhi/material_shader_names.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

using core::EnumName;
using core::JsonFields;
using json = nlohmann::json;
namespace material_shaders = rhi::material_shaders;

constexpr EnumName<PrimitiveShape> k_primitive_shapes[] = {
    {"cube", PrimitiveShape::cube},         {"sphere", PrimitiveShape::sphere},
    {"plane", PrimitiveShape::plane},       {"cylinder", PrimitiveShape::cylinder},
    {"cone", PrimitiveShape::cone},         {"capsule", PrimitiveShape::capsule}};

[[nodiscard]] float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

} // namespace

namespace detail {

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

// "#rrggbb" (sRGB) or linear [r, g, b] in 0..1.
void read_color3(JsonFields& f, std::string_view key, glm::vec3& out) {
    f.known({key});
    glm::vec4 c(out, 1.0F);
    read_color(f, key, c);
    out = glm::vec3(c);
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

// `shader` (a material shader name; "standard" or "" = built-in) and
// `shader_params` (up to 8 numbers filling the two parameter vec4s in order;
// missing ones are 0).
void read_material_shader(JsonFields& f, Primitive& p) {
    std::string shader = p.shader;
    f.string("shader", shader, material_shaders::max_name_length);
    if (shader == material_shaders::builtin_name) shader.clear();
    if (!shader.empty() && !material_shaders::valid_name(shader)) {
        f.fail("shader", "expected a material shader name (lowercase letters, digits and '_', "
                         "starting with a letter) or \"standard\"");
    } else {
        p.shader = std::move(shader);
    }

    f.known({"shader_params"});
    const json* v = f.raw("shader_params");
    if (v == nullptr || v->is_null()) return;
    std::array<glm::vec4, 2> params{};
    bool ok = v->is_array() && v->size() <= 8;
    for (std::size_t i = 0; ok && i < v->size(); ++i) {
        ok = JsonFields::read_float((*v)[i], params[i / 4][static_cast<glm::length_t>(i % 4)]);
    }
    if (ok) {
        p.shader_params = params;
    } else {
        f.fail("shader_params", "expected an array of at most 8 finite numbers");
    }
}

// The shape + flat material fields shared by `primitive` and `instanced_mesh`.
void read_primitive_fields(JsonFields& f, Primitive& p) {
    f.enumeration("shape", p.shape, k_primitive_shapes);
    f.known({"color"});
    read_color(f, "color", p.color);
    f.number("metallic", p.metallic, 0.0F, 1.0F).number("roughness", p.roughness, 0.0F, 1.0F);
    f.color3("emissive", p.emissive);
    read_material_shader(f, p);
}

[[nodiscard]] json primitive_fields_json(const Primitive& p) {
    json params = json::array();
    for (const glm::vec4& v : p.shader_params) {
        for (int i = 0; i < 4; ++i) params.push_back(v[i]);
    }
    return {{"shape", enum_name(k_primitive_shapes, p.shape)},
            {"color", vec4_json(p.color)},
            {"metallic", p.metallic},
            {"roughness", p.roughness},
            {"emissive", vec3_json(p.emissive)},
            {"shader", p.shader.empty() ? std::string(material_shaders::builtin_name) : p.shader},
            {"shader_params", std::move(params)}};
}

// Editing a physics component invalidates its body / character (or, for a
// joint, only its constraint): resetting the handle makes the physics
// systems' orphan sweep drop the old one and build a new one from the
// edited data on the next tick. A rebuilt body takes its joints with it;
// joint_system rebuilds them.
void invalidate_physics(entt::registry& r, entt::entity e, std::string_view edited) {
    if (edited == "joint") {
        if (auto* j = r.try_get<Joint>(e)) j->constraint = physics::PhysicsWorld::invalid_body;
        return;
    }
    if (auto* rb = r.try_get<RigidBody>(e)) rb->body = physics::PhysicsWorld::invalid_body;
    if (auto* cc = r.try_get<CharacterController>(e)) {
        cc->character = physics::PhysicsWorld::invalid_body;
    }
}

} // namespace detail

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

} // namespace engine::scene
