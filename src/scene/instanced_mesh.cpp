// InstancedMesh collection — see include/engine/scene/instanced_mesh.hpp.
// (GPU-side resolution lives in instanced_mesh_resolve.cpp, so this file
// stays device-free and unit-testable.)

#include "engine/scene/instanced_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "fnv_hash.hpp"

namespace engine::scene {

namespace {

using Planes = std::array<glm::vec4, 6>;

// Gribb-Hartmann planes of a view-projection with Vulkan [0, 1] depth,
// normalised so plane distances are in world units (needed for spheres).
// Planes point inward.
[[nodiscard]] Planes frustum_planes(const glm::mat4& view_proj) {
    const glm::mat4 m = glm::transpose(view_proj);
    Planes planes{m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2]};
    for (glm::vec4& p : planes) {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0F) p /= len;
    }
    return planes;
}

[[nodiscard]] bool sphere_visible(const Planes& planes, const glm::vec3& centre, float radius) {
    for (const glm::vec4& p : planes) {
        if (glm::dot(glm::vec3(p), centre) + p.w < -radius) return false;
    }
    return true;
}

[[nodiscard]] bool is_transparent(const asset::MaterialAsset* material) {
    return material != nullptr &&
           (material->params.flags & (asset::material_blend | asset::material_transmission)) != 0;
}

[[nodiscard]] const asset::MeshAsset* mesh_of(const InstancedMeshPart& part) {
    return part.mesh.valid() ? &*part.mesh : nullptr;
}

[[nodiscard]] const asset::MaterialAsset* material_of(const InstancedMeshPart& part) {
    return part.material.valid() ? &*part.material : nullptr;
}

// Calls emit(model, placement) for every placement of the entity whose world
// bounding sphere intersects the frustum (model = world * placement).
template <typename Emit>
void for_each_visible(const Planes& planes, const Transform& transform, const InstancedMesh& mesh,
                      const InstancedMeshParts& parts, Emit&& emit) {
    const glm::vec4 local_centre(glm::vec3(parts.bounds), 1.0F);
    const float radius = parts.bounds.w;
    for (const InstanceTransform& instance : mesh.instances) {
        const glm::mat4 placement = instance_matrix(instance);
        const glm::mat4 model = transform.world * placement;
        const glm::vec3 centre(model * local_centre);
        const float max_scale2 = std::max({glm::dot(glm::vec3(model[0]), glm::vec3(model[0])),
                                           glm::dot(glm::vec3(model[1]), glm::vec3(model[1])),
                                           glm::dot(glm::vec3(model[2]), glm::vec3(model[2]))});
        if (!sphere_visible(planes, centre, radius * std::sqrt(max_scale2))) continue;
        emit(model, placement);
    }
}

} // namespace

glm::mat4 instance_matrix(const InstanceTransform& instance) {
    glm::mat4 m = glm::mat4_cast(instance.rotation);
    m[0] *= instance.scale.x;
    m[1] *= instance.scale.y;
    m[2] *= instance.scale.z;
    m[3] = glm::vec4(instance.position, 1.0F);
    return m;
}

std::uint64_t instanced_mesh_source_hash(const InstancedMesh& mesh) {
    Fnv64 h;
    h.str(mesh.gltf_path);
    if (mesh.gltf_path.empty()) {
        const Primitive& p = mesh.primitive;
        h.u32(static_cast<std::uint32_t>(p.shape));
        for (int i = 0; i < 4; ++i) h.f32(p.color[i]);
        h.f32(p.metallic);
        h.f32(p.roughness);
        for (int i = 0; i < 3; ++i) h.f32(p.emissive[i]);
    } else {
        h.str(mesh.gltf_node);
    }
    return h.nonzero();
}

glm::vec4 instanced_parts_bounds(std::span<const InstancedMeshPart> parts) {
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    bool any = false;
    for (const InstancedMeshPart& part : parts) {
        const asset::MeshAsset* mesh = mesh_of(part);
        if (mesh == nullptr) continue;
        const asset::Aabb& b = mesh->bounds;
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p((corner & 1) != 0 ? b.max.x : b.min.x,
                              (corner & 2) != 0 ? b.max.y : b.min.y,
                              (corner & 4) != 0 ? b.max.z : b.min.z);
            const glm::vec3 q(part.local * glm::vec4(p, 1.0F));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
            any = true;
        }
    }
    if (!any) return glm::vec4(0.0F);
    return {(lo + hi) * 0.5F, glm::length(hi - lo) * 0.5F};
}

bool instanced_meshes_need_resolve(const entt::registry& registry) {
    for (auto [e, mesh] : registry.view<const InstancedMesh>().each()) {
        const auto* parts = registry.try_get<InstancedMeshParts>(e);
        if (parts == nullptr || parts->resolved != instanced_mesh_source_hash(mesh)) return true;
    }
    return false;
}

std::size_t append_instanced_draws(const entt::registry& registry,
                                   const glm::mat4& cull_view_proj, InstancedPass pass,
                                   renderer::InstancedDrawList& out,
                                   renderer::MotionHistory* motion) {
    struct Bucket {
        const asset::MeshAsset*              mesh = nullptr;
        const asset::MaterialAsset*          material = nullptr;
        std::vector<renderer::InstanceData>  instances;
        std::vector<renderer::InstanceMotion> motion;
    };
    std::vector<Bucket> buckets;
    std::vector<int>    part_bucket; // per part of the current entity; -1 = not drawn here
    const Planes planes = frustum_planes(cull_view_proj);
    const bool with_motion = out.motion.size() == out.instances.size();

    for (auto [e, transform, mesh, parts] :
         registry.view<const Transform, const InstancedMesh, const InstancedMeshParts>().each()) {
        if (mesh.instances.empty() || parts.parts.empty()) continue;
        if (pass == InstancedPass::shadow && !mesh.cast_shadows) continue;

        // Map this entity's parts to shared (mesh, material) buckets once.
        part_bucket.assign(parts.parts.size(), -1);
        bool any = false;
        for (std::size_t i = 0; i < parts.parts.size(); ++i) {
            const asset::MeshAsset* m = mesh_of(parts.parts[i]);
            const asset::MaterialAsset* mat = material_of(parts.parts[i]);
            if (m == nullptr || is_transparent(mat)) continue;
            auto it = std::find_if(buckets.begin(), buckets.end(), [&](const Bucket& b) {
                return b.mesh == m && b.material == mat;
            });
            if (it == buckets.end()) {
                buckets.push_back(Bucket{m, mat, {}, {}});
                it = buckets.end() - 1;
            }
            part_bucket[i] = static_cast<int>(it - buckets.begin());
            any = true;
        }
        if (!any) continue;

        // Recorded before culling, so an entity entering the view still has
        // last frame's transform.
        const glm::mat4 prev_world =
            motion != nullptr
                ? motion->advance(static_cast<std::uint64_t>(entt::to_integral(e)), transform.world)
                : transform.world;
        for_each_visible(planes, transform, mesh, parts, [&](const glm::mat4& model,
                                                             const glm::mat4& placement) {
            for (std::size_t i = 0; i < parts.parts.size(); ++i) {
                if (part_bucket[i] < 0) continue;
                Bucket& b = buckets[static_cast<std::size_t>(part_bucket[i])];
                b.instances.push_back(renderer::make_instance_data(model * parts.parts[i].local));
                if (with_motion) {
                    b.motion.push_back(renderer::make_instance_motion(prev_world * placement *
                                                                      parts.parts[i].local));
                }
            }
        });
    }

    std::size_t appended = 0;
    for (Bucket& b : buckets) {
        if (b.instances.empty()) continue;
        renderer::InstancedDraw draw;
        draw.mesh = b.mesh;
        draw.material = b.material;
        draw.first_instance = static_cast<std::uint32_t>(out.instances.size());
        draw.instance_count = static_cast<std::uint32_t>(b.instances.size());
        out.instances.insert(out.instances.end(), b.instances.begin(), b.instances.end());
        out.motion.insert(out.motion.end(), b.motion.begin(), b.motion.end());
        out.draws.push_back(draw);
        appended += b.instances.size();
    }
    return appended;
}

std::size_t append_instanced_transparent_items(const entt::registry& registry,
                                               const glm::mat4& cull_view_proj,
                                               std::vector<renderer::DrawItem>& out) {
    const std::size_t before = out.size();
    const Planes planes = frustum_planes(cull_view_proj);
    for (auto [e, transform, mesh, parts] :
         registry.view<const Transform, const InstancedMesh, const InstancedMeshParts>().each()) {
        const bool any = std::any_of(parts.parts.begin(), parts.parts.end(), [](const auto& p) {
            return mesh_of(p) != nullptr && is_transparent(material_of(p));
        });
        if (!any || mesh.instances.empty()) continue;
        for_each_visible(planes, transform, mesh, parts, [&](const glm::mat4& model,
                                                             const glm::mat4& /*placement*/) {
            for (const InstancedMeshPart& part : parts.parts) {
                const asset::MaterialAsset* mat = material_of(part);
                if (mesh_of(part) == nullptr || !is_transparent(mat)) continue;
                renderer::DrawItem item;
                item.mesh = mesh_of(part);
                item.material = mat;
                item.model = model * part.local;
                out.push_back(item);
            }
        });
    }
    return out.size() - before;
}

} // namespace engine::scene
