// camera.* (the view camera) and render.screenshot.

#include <cmath>
#include <optional>
#include <string>

#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr float k_max_pitch = 89.0F;

struct ClipLimits {
    float       lo;
    float       hi;
    const char* range; // for error messages
};
constexpr ClipLimits k_near_limits{1e-3F, 10'000.0F, "[0.001, 10000]"};
constexpr ClipLimits k_far_limits{1.0F, 1'000'000.0F, "[1, 1000000]"};
// An implicit cut (camera.set without `cut`): a jump of more than this share
// of the far plane, or a turn of more than k_cut_turn_deg.
constexpr float k_cut_far_fraction = 0.1F;
constexpr float k_cut_turn_deg = 30.0F;

[[nodiscard]] Json pose_json(const CameraPose& p) {
    return Json{{"position", vec3_json(p.position)},
                {"yaw_deg", p.yaw_deg},
                {"pitch_deg", p.pitch_deg},
                {"fov_deg", p.fov_deg},
                {"near", p.near_m},
                {"far", p.far_m},
                {"near_auto", !p.near_override.has_value()},
                {"far_auto", !p.far_override.has_value()}};
}

// A clip-plane override: absent keeps the current one, null returns to the
// derived plane, a number sets it.
void read_clip(core::JsonFields& f, std::string_view key, std::optional<float>& out,
               const ClipLimits& limits) {
    const Json* v = f.raw(key);
    if (v == nullptr) return;
    if (v->is_null()) {
        out.reset();
        return;
    }
    float value = 0.0F;
    if (!core::JsonFields::read_float(*v, value) || !(value >= limits.lo && value <= limits.hi)) {
        f.fail(key, std::string("expected a number in ") + limits.range +
                        " metres, or null for the plane derived from the scene");
        return;
    }
    out = value;
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

// Whether moving from `from` to `to` is a teleport rather than continuous
// motion (camera.set without an explicit `cut`).
[[nodiscard]] bool is_cut(const CameraPose& from, const CameraPose& to) {
    if (glm::length(to.position - from.position) > k_cut_far_fraction * from.far_m) return true;
    const float turn = glm::degrees(
        std::acos(glm::clamp(glm::dot(forward_of(from), forward_of(to)), -1.0F, 1.0F)));
    return turn > k_cut_turn_deg || std::abs(to.fov_deg - from.fov_deg) > k_cut_turn_deg;
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
    read_clip(f, "near", pose.near_override, k_near_limits);
    read_clip(f, "far", pose.far_override, k_far_limits);
    const bool has_cut = f.has("cut");
    f.boolean("cut", pose.cut);
    if (pose.near_override && pose.far_override && *pose.far_override <= *pose.near_override) {
        f.fail("far", "must be greater than near");
    }
    if (has_target && has_angles) f.fail("look_at", "give either look_at or yaw_deg/pitch_deg, not both");
    if (has_target && f.ok()) {
        if (glm::length(target - pose.position) < 1e-4F) {
            f.fail("look_at", "target coincides with the camera position");
        } else {
            aim(pose, target - pose.position);
        }
    }
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_cut) pose.cut = is_cut(ctx.services.camera_get(), pose);
    ctx.services.camera_set(pose);
    return pose_json(ctx.services.camera_get());
}

RpcResult camera_frame(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    const Json* ref = f.raw("entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!camera_available(ctx)) return unavailable("the view camera");

    // The entity's subtree, or every root's (terrain excluded: framing the
    // whole scene should not back off to kilometres of streamed tiles).
    std::optional<asset::Aabb> box;
    if (ref != nullptr && !ref->is_null()) {
        auto e = resolve_entity(r, *ref, "entity");
        if (!e) return std::unexpected(e.error());
        box = subtree_world_bounds(ctx, *e, false);
        if (!box) { // draws nothing (a light, an empty): frame its position
            const glm::vec3 p(world_matrix(r, *e)[3]);
            box = asset::Aabb{p - glm::vec3(1.0F), p + glm::vec3(1.0F)};
        }
    } else {
        for (const entt::entity e : r.view<const scene::Transform>()) {
            if (parent_of(r, e) != entt::null) continue;
            if (const auto b = subtree_world_bounds(ctx, e, false)) {
                box = box ? asset::Aabb{glm::min(box->min, b->min), glm::max(box->max, b->max)}
                          : *b;
            }
        }
    }
    if (!box) box = asset::Aabb{glm::vec3(-2.0F), glm::vec3(2.0F)}; // empty scene: the origin
    const glm::vec3 lo = box->min;
    const glm::vec3 hi = box->max;

    // Keep the heading, look down at least 20 degrees, and back off until the
    // bounding sphere fits the vertical field of view (with a margin).
    CameraPose pose = ctx.services.camera_get();
    pose.pitch_deg = std::min(pose.pitch_deg, -20.0F);
    const glm::vec3 centre = 0.5F * (lo + hi);
    const float radius = std::max(0.25F, 0.5F * glm::length(hi - lo));
    const float half_fov = glm::radians(glm::clamp(pose.fov_deg, 1.0F, 179.0F)) * 0.5F;
    const float distance = radius / std::sin(half_fov) * 1.1F;
    pose.position = centre - forward_of(pose) * distance;
    pose.cut = true;
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
                      "fov_deg, near, far, near_auto, far_auto}.",
                      [ctx](const Json& p) { return camera_get(*ctx, p); });
    server.add_method("camera.set",
                      "Move the view camera: {position?, look_at? | yaw_deg?/pitch_deg?, "
                      "fov_deg?, near?, far? (null = derived from the scene), cut? (default: "
                      "a jump > 10% of far or a turn > 30 degrees)}; returns camera.get.",
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
