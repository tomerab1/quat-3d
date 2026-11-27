#pragma once

// Shared helpers for the navigation tests: scene entities with a world
// Transform (the systems read Transform.world; these bare registries run no
// transform system) and static box colliders baked through
// collect_nav_geometry — the same path the editor's "Build navmesh" uses.

#include <cstdint>
#include <expected>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/core/error.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace quat::test {

inline entt::entity place(entt::registry& r, const glm::vec3& pos) {
    const entt::entity e = r.create();
    auto& t = r.emplace<engine::scene::Transform>(e);
    t.local = glm::translate(glm::mat4(1.0F), pos);
    t.world = t.local;
    return e;
}

// A static box collider (no RigidBody: the editor promotes those to static
// bodies at Play, and the bake treats them as static geometry).
inline entt::entity add_box(entt::registry& r, const glm::vec3& centre,
                            const glm::vec3& half_extents) {
    const entt::entity e = place(r, centre);
    r.emplace<engine::scene::Collider>(e, engine::scene::ColliderShape::box, half_extents,
                                       glm::vec3(0.0F));
    return e;
}

[[nodiscard]] inline std::expected<engine::nav::NavMesh, engine::core::Error>
bake(entt::registry& r) {
    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> indices;
    engine::scene::collect_nav_geometry(r, nullptr, glm::vec3(0.0F), verts, indices);
    return engine::nav::NavMesh::build(verts, indices);
}

[[nodiscard]] inline glm::vec3 position_of(const entt::registry& r, entt::entity e) {
    return glm::vec3(r.get<engine::scene::Transform>(e).world[3]);
}

[[nodiscard]] inline float xz_distance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
}

constexpr float kNavDt = 1.0F / 60.0F;

// A registry + its baked navmesh, stepped through nav_agent_system.
struct NavWorld {
    entt::registry registry;
    engine::nav::NavMesh navmesh;

    void bake() {
        auto baked = quat::test::bake(registry);
        ASSERT_OK(baked);
        navmesh = std::move(*baked);
    }
    entt::entity agent(const glm::vec3& at, float speed = 4.0F) {
        const entt::entity e = place(registry, at);
        registry.emplace<engine::scene::NavAgent>(e).speed = speed;
        return e;
    }
    engine::scene::NavAgent& nav(entt::entity e) {
        return registry.get<engine::scene::NavAgent>(e);
    }
    [[nodiscard]] glm::vec3 pos(entt::entity e) const { return position_of(registry, e); }
    // Steps until `done()` holds or the frame budget runs out; returns the
    // frames stepped.
    template <typename Done>
    int run(int max_frames, Done done) {
        for (int i = 0; i < max_frames; ++i) {
            if (done()) return i;
            engine::scene::nav_agent_system(registry, navmesh, kNavDt);
        }
        return max_frames;
    }
};

} // namespace quat::test
