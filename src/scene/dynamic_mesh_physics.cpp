#include <cstring>

#include <glm/gtc/quaternion.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/dynamic_mesh.hpp"

// Static mesh colliders of DynamicMesh components (ROADMAP R23.2): a Jolt
// MeshShape (static bodies only, CLAUDE.md collider_mesh) built from the
// store's latest CPU version, with the entity's world scale baked into the
// points and the entity as the body's owner, so ray casts map back to it.

namespace engine::scene {

namespace {

constexpr std::uint32_t k_no_body = physics::PhysicsWorld::invalid_body;

std::uint64_t world_hash(const glm::mat4& m) {
    std::uint64_t h = 14695981039346656037ULL;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&m);
    for (std::size_t i = 0; i < sizeof m; ++i) {
        h = (h ^ bytes[i]) * 1099511628211ULL;
    }
    return h;
}

// The rebuild key: mesh version and world transform (moving the entity
// re-places the static body).
std::uint64_t collider_key(std::uint64_t version, const glm::mat4& world) {
    return version == 0 ? 0 : (world_hash(world) ^ (version * 0x9E3779B97F4A7C15ULL)) | 1U;
}

void drop_body(physics::PhysicsWorld& world, DynamicMesh& d) {
    if (d.body != k_no_body) {
        world.remove_body(d.body);
        d.body = k_no_body;
    }
    d.collider_version = 0;
}

void build_body(physics::PhysicsWorld& world, entt::entity e, DynamicMesh& d, const geometry::MeshData& mesh,
                const glm::mat4& model) {
    const glm::vec3 scale{glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                          glm::length(glm::vec3(model[2]))};
    if (scale.x <= 0.0F || scale.y <= 0.0F || scale.z <= 0.0F || mesh.indices.empty()) {
        return;
    }
    const glm::mat3 rotation(glm::vec3(model[0]) / scale.x, glm::vec3(model[1]) / scale.y,
                             glm::vec3(model[2]) / scale.z);
    std::vector<glm::vec3> points;
    points.reserve(mesh.vertices.size());
    for (const asset::Vertex& v : mesh.vertices) {
        points.push_back(v.position * scale);
    }
    const std::uint32_t shape = world.create_mesh(points, mesh.indices);
    if (shape == k_no_body) {
        return;
    }
    physics::PhysicsWorld::BodyParams params;
    params.shape = shape;
    params.position = glm::vec3(model[3]);
    params.rotation = glm::normalize(glm::quat_cast(rotation));
    params.motion = physics::Motion::static_body;
    params.layer = physics::Layer::static_body;
    params.owner = static_cast<std::uint32_t>(entt::to_integral(e));
    d.body = world.add_body(params);
    if (d.body == k_no_body) {
        world.release_shape(shape);
    }
}

} // namespace

void dynamic_mesh_collider_system(entt::registry& registry, physics::PhysicsWorld& world,
                                  const DynamicMeshStore& store) {
    for (auto [e, d, t] : registry.view<DynamicMesh, Transform>().each()) {
        // A duplicated entity copies the original's body id: not ours.
        if (d.body != k_no_body &&
            world.body_owner(d.body) != static_cast<std::uint32_t>(entt::to_integral(e))) {
            d.body = k_no_body;
            d.collider_version = 0;
        }
        const geometry::MeshData* mesh = store.mesh(d.mesh);
        if (d.collider != DynamicCollider::mesh || mesh == nullptr) {
            drop_body(world, d);
            continue;
        }
        const std::uint64_t key = collider_key(store.info(d.mesh)->version, t.world);
        if (key == d.collider_version && d.body != k_no_body) {
            continue;
        }
        drop_body(world, d);
        build_body(world, e, d, *mesh, t.world);
        d.collider_version = key;
    }
}

} // namespace engine::scene
