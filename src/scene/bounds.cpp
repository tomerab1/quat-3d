// Draw bounds — see engine/scene/bounds.hpp.

#include "engine/scene/bounds.hpp"

#include <cmath>
#include <limits>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/primitives.hpp"

namespace engine::scene {

asset::Aabb empty_bounds() {
    return {glm::vec3(std::numeric_limits<float>::max()),
            glm::vec3(std::numeric_limits<float>::lowest())};
}

bool bounds_empty(const asset::Aabb& box) {
    return box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z;
}

void merge_bounds(asset::Aabb& into, const asset::Aabb& box) {
    if (bounds_empty(box)) return;
    into.min = glm::min(into.min, box.min);
    into.max = glm::max(into.max, box.max);
}

void merge_point(asset::Aabb& into, const glm::vec3& p) {
    into.min = glm::min(into.min, p);
    into.max = glm::max(into.max, p);
}

asset::Aabb transform_bounds(const asset::Aabb& box, const glm::mat4& m) {
    if (bounds_empty(box)) return box;
    // Arvo: the centre maps through m, the half extent through |linear part|.
    const glm::vec3 centre = 0.5F * (box.min + box.max);
    const glm::vec3 half = 0.5F * (box.max - box.min);
    const glm::vec3 c(m * glm::vec4(centre, 1.0F));
    glm::vec3 h(0.0F);
    for (int col = 0; col < 3; ++col) {
        h += glm::abs(glm::vec3(m[col])) * half[col];
    }
    return {c - h, c + h};
}

namespace {

// The model of an InstancedMesh in the instance frame: its resolved parts, or
// the primitive's unit shape before (or without) a glTF model.
[[nodiscard]] std::optional<asset::Aabb> instance_model_bounds(const entt::registry& r,
                                                               entt::entity e,
                                                               const InstancedMesh& mesh) {
    asset::Aabb box = empty_bounds();
    if (const auto* parts = r.try_get<InstancedMeshParts>(e); parts != nullptr) {
        for (const InstancedMeshPart& part : parts->parts) {
            if (!part.mesh.valid() || !part.mesh.is_loaded() || !part.mesh->valid()) continue;
            merge_bounds(box, transform_bounds(part.mesh->bounds, part.local));
        }
    }
    if (bounds_empty(box) && mesh.gltf_path.empty()) box = primitive_bounds(mesh.primitive.shape);
    if (bounds_empty(box)) return std::nullopt;
    return box;
}

} // namespace

std::optional<asset::Aabb> local_draw_bounds(const entt::registry& r, entt::entity e) {
    if (!r.valid(e)) return std::nullopt;
    asset::Aabb box = empty_bounds();
    if (const auto* mr = r.try_get<MeshRenderer>(e);
        mr != nullptr && mr->mesh.valid() && mr->mesh.is_loaded() && mr->mesh->valid()) {
        // Skinned meshes are placed through their armature (as the renderer does).
        const auto* sm = r.try_get<SkinnedMesh>(e);
        merge_bounds(box, sm != nullptr ? transform_bounds(mr->mesh->bounds, sm->root_transform)
                                        : mr->mesh->bounds);
    } else if (const auto* p = r.try_get<Primitive>(e)) {
        merge_bounds(box, primitive_bounds(p->shape)); // not resolved yet
    }
    if (const auto* im = r.try_get<InstancedMesh>(e); im != nullptr && !im->instances.empty()) {
        if (const auto model = instance_model_bounds(r, e, *im)) {
            for (const InstanceTransform& inst : im->instances) {
                merge_bounds(box, transform_bounds(*model, instance_matrix(inst)));
            }
        }
    }
    if (bounds_empty(box)) return std::nullopt;
    return box;
}

std::optional<asset::Aabb> scene_draw_bounds(const entt::registry& r) {
    asset::Aabb box = empty_bounds();
    for (const auto [e, t] : r.view<const Transform>().each()) {
        if (const auto local = local_draw_bounds(r, e)) {
            merge_bounds(box, transform_bounds(*local, t.world));
        }
    }
    if (bounds_empty(box)) return std::nullopt;
    return box;
}

} // namespace engine::scene
