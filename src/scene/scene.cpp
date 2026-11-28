// Scene + system scheduler (Phase 4, Slice 4.2).

#include "engine/scene/scene.hpp"
#include "engine/weather/weather_state.hpp"
#include "engine/scene/world_clock.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/profile.hpp"
#include "engine/renderer/draw_batching.hpp"

namespace engine::scene {

namespace {

// Recursively resolve world = parent_world * local down the Children hierarchy.
void propagate(entt::registry& reg, entt::entity e, const glm::mat4& parent_world) {
    auto& t = reg.get<Transform>(e);
    t.world = parent_world * t.local;
    if (const auto* kids = reg.try_get<Children>(e)) {
        for (entt::entity c : kids->entities) {
            if (reg.valid(c) && reg.all_of<Transform>(c)) {
                propagate(reg, c, t.world);
            }
        }
    }
}

} // namespace

Scene::Scene()                           = default;
Scene::~Scene()                          = default;
Scene::Scene(Scene&&) noexcept           = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

entt::entity Scene::create_entity(std::string name) {
    const entt::entity e = registry_.create();
    registry_.emplace<Name>(e, std::move(name));
    registry_.emplace<Transform>(e);
    return e;
}

bool Scene::set_parent(entt::entity child, entt::entity parent) {
    if (!registry_.valid(child) || child == parent) return false;
    if (parent != entt::null) {
        if (!registry_.valid(parent)) return false;
        // Walk up from the new parent: reaching `child` means a cycle. The
        // walk is bounded so a pre-existing corrupt chain cannot loop forever.
        std::size_t guard = 0;
        for (entt::entity a = parent; a != entt::null && guard < 1'000'000; ++guard) {
            if (a == child) return false;
            const auto* up = registry_.try_get<Parent>(a);
            a = up != nullptr && registry_.valid(up->entity) ? up->entity : entt::null;
        }
    }

    // Detach from the current parent's child list, if any.
    if (auto* cur = registry_.try_get<Parent>(child);
        cur != nullptr && cur->entity != entt::null) {
        if (auto* kids = registry_.try_get<Children>(cur->entity)) {
            auto& v = kids->entities;
            v.erase(std::remove(v.begin(), v.end(), child), v.end());
        }
    }

    if (parent == entt::null) {
        registry_.remove<Parent>(child);
        return true;
    }
    registry_.emplace_or_replace<Parent>(child, parent);
    registry_.get_or_emplace<Children>(parent).entities.push_back(child);
    return true;
}

void Scene::tick(float dt, const FootGroundProbe& ground, const SceneTickHooks& hooks) {
    // Transforms first: the animation system folds each skinned entity's
    // current Transform.world into its skinning matrices, so worlds must be
    // up to date before sampling (animation writes no transforms itself).
    {
        QUAT_ZONE_C("transform_system", core::ProfileCategory::system);
        transform_system(registry_);
    }
    if (hooks.post_transform) hooks.post_transform(dt);
    {
        QUAT_ZONE_C("animation_system", core::ProfileCategory::system);
        animation_system(registry_, dt, ground);
    }
    {
        QUAT_ZONE_C("weather_system", core::ProfileCategory::system);
        weather::weather_system(registry_, dt); // wetness / snow accumulation (R13.1)
    }
    {
        QUAT_ZONE_C("world_clock_system", core::ProfileCategory::system);
        world_clock_system(registry_, dt); // date / time, sun and sky (R22.1)
    }
    if (hooks.pre_render) hooks.pre_render(dt);
    QUAT_ZONE_C("render_collect_system", core::ProfileCategory::system);
    render_collect_system(registry_, draw_list_);
}

entt::entity Scene::active_camera() const {
    for (auto [e, cam] : registry_.view<const Camera>().each()) {
        if (cam.is_active) return e;
    }
    return entt::null;
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

void set_world_transform(entt::registry& registry, entt::entity e, const glm::mat4& world) {
    auto* t = registry.try_get<Transform>(e);
    if (t == nullptr) return;
    glm::mat4 parent_world(1.0F);
    if (const auto* p = registry.try_get<Parent>(e);
        p != nullptr && p->entity != entt::null && registry.valid(p->entity)) {
        if (const auto* pt = registry.try_get<Transform>(p->entity)) parent_world = pt->world;
    }
    t->world = world;
    t->local = glm::inverse(parent_world) * world;
}

void transform_system(entt::registry& registry) {
    const glm::mat4 identity(1.0F);
    for (entt::entity e : registry.view<Transform>()) {
        const auto* p = registry.try_get<Parent>(e);
        const bool is_root = (p == nullptr) || (p->entity == entt::null);
        if (is_root) {
            propagate(registry, e, identity);
        }
    }
}

void render_collect_system(const entt::registry& registry,
                           std::vector<renderer::DrawItem>& out) {
    out.clear();
    for (auto [e, t, mr] : registry.view<const Transform, const MeshRenderer>().each()) {
        renderer::DrawItem item;
        item.model = t.world;
        item.mesh = mr.mesh.valid() ? &*mr.mesh : nullptr;
        item.material = mr.material.valid() ? &*mr.material : nullptr;
        out.push_back(item);
    }
}

entt::entity find_active_camera(const entt::registry& registry) {
    for (auto [e, cam] : registry.view<const Camera>().each()) {
        if (cam.is_active && registry.all_of<Transform>(e)) {
            return e;
        }
    }
    return entt::null;
}

renderer::PhysicalCamera active_camera_lens(const entt::registry& registry,
                                            const renderer::PhysicalCamera& fallback) {
    const entt::entity e = find_active_camera(registry);
    return e == entt::null ? fallback : registry.get<const Camera>(e).lens;
}

CameraMatrices camera_system(const entt::registry& registry, float aspect_ratio) {
    CameraMatrices out;

    const entt::entity active = find_active_camera(registry);
    if (active == entt::null) {
        return out;
    }

    const auto& cam = registry.get<const Camera>(active);
    const glm::mat4& world = registry.get<const Transform>(active).world;

    out.position   = glm::vec3(world[3]);
    out.view       = glm::inverse(world);
    out.projection = glm::perspective(renderer::effective_fov_y(cam.lens, cam.fov_y),
                                      aspect_ratio, cam.near_z, cam.far_z);
    out.projection[1][1] *= -1.0F; // Vulkan clip space: Y points down
    out.view_proj  = out.projection * out.view;
    return out;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

std::expected<void, core::Error> run_scene_self_test() {
    Scene scene;

    // Parent translated +X by 1; child translated +Y by 2, parented under it.
    // After propagation the child's world translation must be (1, 2, 0).
    const entt::entity parent = scene.create_entity("parent");
    scene.registry().get<Transform>(parent).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 0.0F, 0.0F));
    scene.registry().emplace<Camera>(parent);

    const entt::entity child = scene.create_entity("child");
    scene.registry().get<Transform>(child).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 0.0F));
    scene.registry().emplace<MeshRenderer>(child);
    scene.set_parent(child, parent);

    scene.tick();

    const glm::vec3 child_world = glm::vec3(scene.registry().get<Transform>(child).world[3]);
    const glm::vec3 expected(1.0F, 2.0F, 0.0F);
    if (glm::distance(child_world, expected) > 1e-4F) {
        return std::unexpected(core::Error{"scene self-test: child world translation wrong"});
    }

    // Parent stays at its own local translation (it is a root).
    const glm::vec3 parent_world = glm::vec3(scene.registry().get<Transform>(parent).world[3]);
    if (glm::distance(parent_world, glm::vec3(1.0F, 0.0F, 0.0F)) > 1e-4F) {
        return std::unexpected(core::Error{"scene self-test: parent world translation wrong"});
    }

    // The child is the only renderable; the active camera resolves to the parent.
    if (scene.draw_list().size() != 1) {
        return std::unexpected(core::Error{"scene self-test: draw list should hold one item"});
    }
    if (scene.active_camera() != parent) {
        return std::unexpected(core::Error{"scene self-test: active camera not found"});
    }

    // Re-parenting to null detaches and clears the parent's child list.
    scene.set_parent(child, entt::null);
    if (scene.registry().all_of<Parent>(child)) {
        return std::unexpected(core::Error{"scene self-test: detach left a Parent component"});
    }
    if (!scene.registry().get<Children>(parent).entities.empty()) {
        return std::unexpected(core::Error{"scene self-test: detach left a stale child entry"});
    }

    scene.tick();
    const glm::vec3 detached_world = glm::vec3(scene.registry().get<Transform>(child).world[3]);
    if (glm::distance(detached_world, glm::vec3(0.0F, 2.0F, 0.0F)) > 1e-4F) {
        return std::unexpected(core::Error{"scene self-test: detached child did not become a root"});
    }

    // Cycles are refused (S1 audit B13): self-parenting, parenting under a
    // descendant, and invalid entities leave the hierarchy untouched.
    {
        const entt::entity a = scene.create_entity("a");
        const entt::entity b = scene.create_entity("b");
        const entt::entity c = scene.create_entity("c");
        if (!scene.set_parent(b, a) || !scene.set_parent(c, b)) {
            return std::unexpected(core::Error{"scene self-test: valid re-parent refused"});
        }
        if (scene.set_parent(a, a) || scene.set_parent(a, c) || scene.set_parent(a, b)) {
            return std::unexpected(core::Error{"scene self-test: cyclic re-parent accepted"});
        }
        const entt::entity gone = scene.create_entity("gone");
        scene.registry().destroy(gone);
        if (scene.set_parent(a, gone) || scene.set_parent(gone, a)) {
            return std::unexpected(core::Error{"scene self-test: invalid entity re-parent accepted"});
        }
        if (scene.registry().all_of<Parent>(a) ||
            scene.registry().get<Parent>(c).entity != b ||
            scene.registry().get<Children>(a).entities.size() != 1) {
            return std::unexpected(core::Error{"scene self-test: refused re-parent changed links"});
        }
        scene.registry().destroy(a);
        scene.registry().destroy(b);
        scene.registry().destroy(c);
    }

    // Camera at +5 Z, identity orientation: the view matrix is the inverse of its
    // world transform, so it translates the world by -5 Z and reports position +5 Z.
    scene.registry().get<Transform>(parent).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 5.0F));
    scene.tick();
    const CameraMatrices cam = camera_system(scene.registry(), 16.0F / 9.0F);
    if (glm::distance(glm::vec3(cam.view[3]), glm::vec3(0.0F, 0.0F, -5.0F)) > 1e-4F) {
        return std::unexpected(core::Error{"scene self-test: camera view translation wrong"});
    }
    if (glm::distance(cam.position, glm::vec3(0.0F, 0.0F, 5.0F)) > 1e-4F) {
        return std::unexpected(core::Error{"scene self-test: camera position wrong"});
    }

    return {};
}

} // namespace engine::scene
