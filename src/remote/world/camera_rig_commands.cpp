// camera.rig.* (ROADMAP G15.1): read, drive and frame an entity's camera rig.
// See docs/remote-control.md and docs/camera-rig.md.

#include <nlohmann/json.hpp>

#include "engine/renderer/physical_camera.hpp"
#include "engine/scene/camera_rig.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr float k_default_fov = 1.0471976F; // 60 degrees
constexpr float k_default_aspect = 16.0F / 9.0F;

// The rig named by `entity`, or the scene's only / active-camera rig.
[[nodiscard]] std::expected<entt::entity, RpcError> rig_entity(const Context& ctx, const Json* ref) {
    const entt::registry& r = ctx.scene->registry();
    if (ref != nullptr && !ref->is_null()) {
        auto e = resolve_entity(r, *ref, "entity");
        if (!e) return std::unexpected(e.error());
        return *e;
    }
    entt::entity found = entt::null;
    for (const entt::entity e : r.view<const scene::CameraRig>()) {
        const auto* cam = r.try_get<scene::Camera>(e);
        if (cam != nullptr && cam->is_active) return e; // the rig the view looks through
        if (found == entt::null) found = e;
    }
    if (found == entt::null) {
        return rpc_fail(rpc_code::not_found,
                        "no entity has a camera_rig (add one: scene.update {entity, components: {camera_rig: {}}})");
    }
    return found;
}

[[nodiscard]] Json rig_json(const Context& ctx, entt::entity e) {
    const entt::registry& r = ctx.scene->registry();
    const std::string only[] = {"camera_rig", "camera_rig_input"};
    scene::EncodeOptions opts;
    opts.only = only;
    Json out = scene::encode_components(r, e, opts);
    out["entity"] = entity_json(e);
    return out;
}

// Snaps the rig's damped state to its targets and places the entity there.
void snap(Context& ctx, entt::entity e) {
    entt::registry& r = ctx.scene->registry();
    auto& rig = r.get<scene::CameraRig>(e);
    scene::camera_rig_snap(rig);
    if (auto* t = r.try_get<scene::Transform>(e)) {
        t->local = scene::camera_rig_matrix(rig);
        refresh_world(r, e);
    }
}

RpcResult rig_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    const Json* ref = f.raw("entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto e = rig_entity(ctx, ref);
    if (!e) return std::unexpected(e.error());
    if (!ctx.scene->registry().all_of<scene::CameraRig>(*e)) {
        return rpc_fail(rpc_code::not_found, "entity has no camera_rig");
    }
    return rig_json(ctx, *e);
}

// {entity?, immediate?=false, <camera_rig fields>}: fields as the
// camera_rig component (focus, yaw_deg, pitch_deg, distance, limits, ...).
RpcResult rig_set(Context& ctx, const Json& params) {
    if (!params.is_object()) return bad_params("expected an object");
    const Json* ref = params.contains("entity") ? &params["entity"] : nullptr;
    bool immediate = false;
    Json fields = params;
    fields.erase("entity");
    if (params.contains("immediate")) {
        if (!params["immediate"].is_boolean()) return bad_params("immediate: expected true or false");
        immediate = params["immediate"].get<bool>();
        fields.erase("immediate");
    }
    auto e = rig_entity(ctx, ref);
    if (!e) return std::unexpected(e.error());
    entt::registry& r = ctx.scene->registry();
    if (auto ok = scene::apply_component(r, *e, "camera_rig", fields, true); !ok) {
        return bad_params(ok.error().message);
    }
    if (immediate) snap(ctx, *e);
    return rig_json(ctx, *e);
}

// {entity?, target? (entity; the whole scene when absent), margin?=0.1, immediate?=false}.
RpcResult rig_frame(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    const Json* ref = f.raw("entity");
    const Json* target = f.raw("target");
    float margin = 0.1F;
    bool immediate = false;
    f.number("margin", margin, 0.0F, 0.9F).boolean("immediate", immediate);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    auto e = rig_entity(ctx, ref);
    if (!e) return std::unexpected(e.error());
    entt::registry& r = ctx.scene->registry();
    if (!r.all_of<scene::CameraRig>(*e)) return rpc_fail(rpc_code::not_found, "entity has no camera_rig");

    std::optional<asset::Aabb> box;
    if (target != nullptr && !target->is_null()) {
        auto t = resolve_entity(r, *target, "target");
        if (!t) return std::unexpected(t.error());
        box = subtree_world_bounds(ctx, *t, false);
        if (!box) {
            const glm::vec3 p(world_matrix(r, *t)[3]);
            box = asset::Aabb{p - glm::vec3(1.0F), p + glm::vec3(1.0F)};
        }
    } else {
        for (const entt::entity root : r.view<const scene::Transform>()) {
            if (root == *e || parent_of(r, root) != entt::null) continue;
            if (const auto b = subtree_world_bounds(ctx, root, false)) {
                box = box ? asset::Aabb{glm::min(box->min, b->min), glm::max(box->max, b->max)} : *b;
            }
        }
    }
    if (!box) return rpc_fail(rpc_code::not_found, "nothing to frame: the scene draws nothing");
    const auto* cam = r.try_get<scene::Camera>(*e);
    const float aspect = ctx.services.view_aspect ? ctx.services.view_aspect() : k_default_aspect;
    scene::camera_rig_frame(r.get<scene::CameraRig>(*e), box->min, box->max,
                            cam != nullptr ? renderer::effective_fov_y(cam->lens, cam->fov_y) : k_default_fov,
                            aspect > 0.0F ? aspect : k_default_aspect, margin);
    if (immediate) snap(ctx, *e);
    return rig_json(ctx, *e);
}

} // namespace

void register_camera_rig_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("camera.rig.get",
                      "A camera rig: {entity? (default: the active camera's rig)} -> {entity, camera_rig "
                      "(targets + current pose), camera_rig_input?}.",
                      [ctx](const Json& p) { return rig_get(*ctx, p); });
    server.add_method("camera.rig.set",
                      "Move a camera rig's targets: {entity?, immediate? (skip smoothing), focus?, yaw_deg?, "
                      "pitch_deg?, distance?, ...camera_rig fields}.",
                      [ctx](const Json& p) { return rig_set(*ctx, p); });
    server.add_method("camera.rig.frame",
                      "Frame an entity (or the whole scene) with a camera rig at its yaw: {entity?, target?, "
                      "margin?=0.1, immediate?}.",
                      [ctx](const Json& p) { return rig_frame(*ctx, p); });
}

} // namespace engine::remote::world
