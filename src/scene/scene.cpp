// Scene + system scheduler (Phase 4, Slice 4.2).

#include "engine/scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/animation/animator.hpp"

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

void Scene::tick(float dt) {
    // Transforms first: the animation system folds each skinned entity's
    // current Transform.world into its skinning matrices, so worlds must be
    // up to date before sampling (animation writes no transforms itself).
    transform_system(registry_);
    animation_system(registry_, dt);
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

void animation_system(entt::registry& registry, float dt) {
    for (auto [e, anim, skinned, transform] :
         registry.view<Animator, SkinnedMesh, Transform>().each()) {
        if (!skinned.skeleton.valid() || !skinned.skeleton.is_loaded()) {
            continue;
        }
        const animation::SkeletonAsset& skeleton = *skinned.skeleton;

        // The armature root in world space: the entity's current world transform
        // (fresh — transform_system runs first) times the load-time offset from
        // the entity's frame to the armature. Skinned draws use an identity
        // model matrix, so this is what places (and moves) the model.
        const glm::mat4 root = transform.world * skinned.root_transform;

        if (anim.clip.valid() && anim.clip.is_loaded() && anim.clip->valid()) {
            const animation::AnimClipAsset& clip = *anim.clip;
            anim.time += dt * anim.speed;
            if (clip.duration > 0.0F) {
                if (anim.looping) {
                    anim.time = std::fmod(anim.time, clip.duration);
                    if (anim.time < 0.0F) anim.time += clip.duration;
                } else {
                    anim.time = std::clamp(anim.time, 0.0F, clip.duration);
                }
            }
            skinned.joint_matrices =
                animation::sample_skinning_matrices(skeleton, clip, anim.time, root);
        } else {
            skinned.joint_matrices = animation::bind_skinning_matrices(skeleton, root);
        }
    }
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
    out.projection = glm::perspective(cam.fov_y, aspect_ratio, cam.near_z, cam.far_z);
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
