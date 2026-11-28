// Codec component `transform`: Local position / rotation (Euler degrees, YXZ) / scale.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#define GLM_ENABLE_EXPERIMENTAL
#include <cmath>
#include <expected>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

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

} // namespace

std::expected<void, core::Error> register_transform(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "transform",
                         .summary = "Local position / rotation (Euler degrees, YXZ) / scale",
                         .addable = false,
                         .writable = true,
                         .order = 100};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& options) {
        json out;
        const auto* t = &r.get<Transform>(e);
        out["transform"] = encode_transform(*t, options.transform_as_matrix);
        return std::move(out["transform"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        Transform t = r.all_of<Transform>(e) ? r.get<Transform>(e) : Transform{};
        if (auto ok = decode_transform(t, v, strict); !ok) return ok;
        r.emplace_or_replace<Transform>(e, t);
        return {};
    };
    codec.has = [](const entt::registry& r, entt::entity e) { return r.all_of<Transform>(e); };
    return registry.add(std::move(codec)); // not removable: no `remove`
}

} // namespace engine::scene::codec
