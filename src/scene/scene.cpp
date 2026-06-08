// Scene + system scheduler (Phase 4, Slice 4.2).

#include "engine/scene/scene.hpp"

#include <algorithm>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

entt::entity Scene::create_entity(std::string name) {
    const entt::entity e = registry_.create();
    registry_.emplace<Name>(e, std::move(name));
    registry_.emplace<Transform>(e);
    return e;
}

void Scene::set_parent(entt::entity child, entt::entity parent) {
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
        return;
    }
    registry_.emplace_or_replace<Parent>(child, parent);
    registry_.get_or_emplace<Children>(parent).entities.push_back(child);
}

void Scene::tick() {
    transform_system(registry_);
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

    return {};
}

} // namespace engine::scene
