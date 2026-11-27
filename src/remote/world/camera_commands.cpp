// camera.* (the view camera) and render.screenshot.

#include <cmath>
#include <limits>
#include <string>

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/primitives.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr float k_max_pitch = 89.0F;

[[nodiscard]] Json lens_json(const renderer::PhysicalCamera& l) {
    return Json{{"aperture_f", l.aperture_f},
                {"shutter_s", l.shutter_s},
                {"iso", l.iso},
                {"sensor_height_mm", l.sensor_height_mm},
                {"focal_length_mm", l.focal_length_mm},
                {"focus_distance_m", l.focus_distance_m}};
}

[[nodiscard]] Json pose_json(const CameraPose& p) {
    return Json{{"position", vec3_json(p.position)},
                {"yaw_deg", p.yaw_deg},
                {"pitch_deg", p.pitch_deg},
                {"fov_deg", p.fov_deg},
                {"lens", lens_json(p.lens)}};
}

// Merges the optional `lens` object (R20) into `lens`; fields it omits keep
// their values. Errors land in `f` under "lens.<field>".
void read_lens(core::JsonFields& f, renderer::PhysicalCamera& lens) {
    const Json* v = f.raw("lens");
    if (v == nullptr || v->is_null()) return;
    if (!v->is_object()) {
        f.fail("lens", "expected an object of lens fields");
        return;
    }
    using L = renderer::PhysicalCameraLimits;
    core::JsonFields l(*v, "lens");
    l.number("aperture_f", lens.aperture_f, L::min_aperture, L::max_aperture)
        .number("shutter_s", lens.shutter_s, L::min_shutter, L::max_shutter)
        .number("iso", lens.iso, L::min_iso, L::max_iso)
        .number("sensor_height_mm", lens.sensor_height_mm, L::min_sensor_mm, L::max_sensor_mm)
        .number("focal_length_mm", lens.focal_length_mm, 0.0F, L::max_focal_mm)
        .number("focus_distance_m", lens.focus_distance_m, L::min_focus_m, L::max_focus_m);
    if (auto ok = finish(l); !ok) f.fail("lens", ok.error().message);
}

[[nodiscard]] glm::vec3 forward_of(const CameraPose& p) {
    const float yaw = glm::radians(p.yaw_deg);
    const float pitch = glm::radians(p.pitch_deg);
    return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}

void aim(CameraPose& p, const glm::vec3& dir) {
    const glm::vec3 d = glm::normalize(dir);
    p.yaw_deg = glm::degrees(std::atan2(d.z, d.x));
    p.pitch_deg = glm::clamp(glm::degrees(std::asin(glm::clamp(d.y, -1.0F, 1.0F))), -k_max_pitch,
                             k_max_pitch);
}

[[nodiscard]] bool camera_available(const Context& ctx) {
    return ctx.services.camera_get && ctx.services.camera_set;
}

RpcResult camera_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!camera_available(ctx)) return unavailable("the view camera");
    return pose_json(ctx.services.camera_get());
}

RpcResult camera_set(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (!camera_available(ctx)) return unavailable("the view camera");
    CameraPose pose = ctx.services.camera_get();
    glm::vec3 target(0.0F);
    const bool has_target = f.has("look_at");
    const bool has_angles = f.has("yaw_deg") || f.has("pitch_deg");
    f.vec3("position", pose.position).vec3("look_at", target);
    f.number("yaw_deg", pose.yaw_deg, -360.0F, 360.0F);
    f.number("pitch_deg", pose.pitch_deg, -k_max_pitch, k_max_pitch);
    f.number("fov_deg", pose.fov_deg, 1.0F, 179.0F);
    read_lens(f, pose.lens);
    if (has_target && has_angles) f.fail("look_at", "give either look_at or yaw_deg/pitch_deg, not both");
    if (has_target && f.ok()) {
        if (glm::length(target - pose.position) < 1e-4F) {
            f.fail("look_at", "target coincides with the camera position");
        } else {
            aim(pose, target - pose.position);
        }
    }
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    ctx.services.camera_set(pose);
    return pose_json(ctx.services.camera_get());
}

// World-space bounds of what an entity draws (mesh bounds, primitive shape),
// grown into `lo`/`hi`. Returns false when the entity draws nothing.
bool grow_bounds(const entt::registry& r, entt::entity e, glm::vec3& lo, glm::vec3& hi) {
    asset::Aabb box{};
    glm::mat4 model = world_matrix(r, e);
    if (const auto* mr = r.try_get<scene::MeshRenderer>(e);
        mr != nullptr && mr->mesh.valid() && mr->mesh.is_loaded() && mr->mesh->valid()) {
        box = mr->mesh->bounds;
        // Skinned meshes are placed through their armature (as the renderer does).
        if (const auto* sm = r.try_get<scene::SkinnedMesh>(e)) model = model * sm->root_transform;
    } else if (const auto* p = r.try_get<scene::Primitive>(e)) {
        box = scene::primitive_bounds(p->shape);
    } else {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                               (i & 4) ? box.max.z : box.min.z);
        const glm::vec3 w(model * glm::vec4(corner, 1.0F));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
    return true;
}

RpcResult camera_frame(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    const Json* ref = f.raw("entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!camera_available(ctx)) return unavailable("the view camera");

    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    bool any = false;
    if (ref != nullptr && !ref->is_null()) {
        auto e = resolve_entity(r, *ref, "entity");
        if (!e) return std::unexpected(e.error());
        for (const entt::entity c : subtree(r, *e)) any = grow_bounds(r, c, lo, hi) || any;
        if (!any) { // draws nothing (a light, an empty): frame its position
            const glm::vec3 p(world_matrix(r, *e)[3]);
            lo = p - glm::vec3(1.0F);
            hi = p + glm::vec3(1.0F);
            any = true;
        }
    } else {
        for (const entt::entity e : r.view<const scene::Transform>()) {
            any = grow_bounds(r, e, lo, hi) || any;
        }
    }
    if (!any) { // empty scene: look at the origin
        lo = glm::vec3(-2.0F);
        hi = glm::vec3(2.0F);
    }

    // Keep the heading, look down at least 20 degrees, and back off until the
    // bounding sphere fits the vertical field of view (with a margin).
    CameraPose pose = ctx.services.camera_get();
    pose.pitch_deg = std::min(pose.pitch_deg, -20.0F);
    const glm::vec3 centre = 0.5F * (lo + hi);
    const float radius = std::max(0.25F, 0.5F * glm::length(hi - lo));
    const float half_fov = glm::radians(glm::clamp(pose.fov_deg, 1.0F, 179.0F)) * 0.5F;
    const float distance = radius / std::sin(half_fov) * 1.1F;
    pose.position = centre - forward_of(pose) * distance;
    ctx.services.camera_set(pose);
    return pose_json(ctx.services.camera_get());
}

void screenshot(Context& ctx, const Json& params, Responder responder) {
    core::JsonFields f(params, "");
    std::uint32_t max_size = 1024;
    f.integer("max_size", max_size, 16, 8192);
    if (auto ok = finish(f); !ok) {
        responder.reply(std::unexpected(ok.error()));
        return;
    }
    if (!ctx.services.screenshot) {
        responder.reply(unavailable("rendering (screenshots)"));
        return;
    }
    ctx.services.screenshot(max_size, std::move(responder));
}

} // namespace

void register_camera_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("camera.get",
                      "View camera: {position, yaw_deg (0 = +X, 90 = +Z), pitch_deg (up > 0), "
                      "fov_deg, lens}.",
                      [ctx](const Json& p) { return camera_get(*ctx, p); });
    server.add_method("camera.set",
                      "Move the view camera: {position?, look_at? | yaw_deg?/pitch_deg?, "
                      "fov_deg?, lens?}; returns camera.get.",
                      [ctx](const Json& p) { return camera_set(*ctx, p); });
    server.add_method("camera.frame",
                      "Point the view camera at an entity (and its children) or, without "
                      "{entity}, the whole scene; returns camera.get.",
                      [ctx](const Json& p) { return camera_frame(*ctx, p); });
    server.add_deferred_method(
        "render.screenshot",
        "PNG of the next rendered frame (the scene view, without editor UI): {max_size?=1024} -> "
        "{mime, width, height, data (base64)}.",
        [ctx](const Json& p, Responder r) { screenshot(*ctx, p, std::move(r)); });
}

} // namespace engine::remote::world
