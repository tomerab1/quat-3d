// ECS self-test for the core components (Phase 4, Slice 4.1).

#include "engine/scene/components.hpp"

#include <entt/entt.hpp>

namespace engine::scene {

std::expected<void, core::Error> run_ecs_self_test() {
    entt::registry registry;

    // A small hierarchy: a root with a camera + light, and a child mesh.
    const entt::entity root = registry.create();
    registry.emplace<Name>(root, "root");
    registry.emplace<Transform>(root);
    registry.emplace<Camera>(root);
    registry.emplace<DirectionalLight>(root,
                                       glm::vec3{0.0F, -1.0F, 0.0F}, glm::vec3{1.0F}, 2.0F);
    registry.emplace<Children>(root);

    const entt::entity child = registry.create();
    registry.emplace<Name>(child, "child");
    registry.emplace<Transform>(child);
    registry.emplace<MeshRenderer>(child);
    registry.emplace<Parent>(child, root);
    registry.get<Children>(root).entities.push_back(child);

    if (registry.view<Transform>().size() != 2) {
        return std::unexpected(core::Error{"ecs self-test: expected 2 entities with Transform"});
    }

    // View over the renderables: exactly the child carries Transform+MeshRenderer
    // together as a draw candidate (the root has Transform but no MeshRenderer).
    int renderable_count = 0;
    entt::entity seen = entt::null;
    for (auto entity : registry.view<Transform, MeshRenderer>()) {
        ++renderable_count;
        seen = entity;
    }
    if (renderable_count != 1 || seen != child) {
        return std::unexpected(core::Error{"ecs self-test: renderable view mismatch"});
    }

    // The active camera is reachable and its parameters round-trip.
    int camera_count = 0;
    for (auto [entity, cam] : registry.view<Camera>().each()) {
        if (cam.is_active) ++camera_count;
    }
    if (camera_count != 1) {
        return std::unexpected(core::Error{"ecs self-test: expected 1 active camera"});
    }

    // Hierarchy links resolve both ways.
    if (registry.get<Parent>(child).entity != root) {
        return std::unexpected(core::Error{"ecs self-test: child->parent link broken"});
    }
    const auto& kids = registry.get<Children>(root).entities;
    if (kids.size() != 1 || kids.front() != child) {
        return std::unexpected(core::Error{"ecs self-test: parent->child link broken"});
    }

    // Light parameters round-trip through the registry.
    if (registry.get<DirectionalLight>(root).intensity != 2.0F) {
        return std::unexpected(core::Error{"ecs self-test: light intensity not preserved"});
    }

    registry.destroy(child);
    if (registry.view<Transform>().size() != 1) {
        return std::unexpected(core::Error{"ecs self-test: destroy did not free entity"});
    }

    return {};
}

} // namespace engine::scene
