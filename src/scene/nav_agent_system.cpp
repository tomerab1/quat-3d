// NavAgent path following + separation steering (Phase 13, Slice 13.2).

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/nav/navmesh.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

constexpr float kWaypointReach = 0.6F;  // advance when this close (xz)
constexpr float kArriveReach = 0.4F;    // final waypoint tolerance
constexpr float kRepathDistance = 0.5F; // target moved farther -> repath
constexpr float kSeparation = 1.2F;     // agent-agent comfort radius

[[nodiscard]] glm::vec2 xz(const glm::vec3& v) { return {v.x, v.z}; }

} // namespace

void nav_agent_system(entt::registry& registry, const nav::NavMesh& navmesh, float dt) {
    if (!navmesh.valid()) return;

    // Patrol routes feed the agent target point by point: advance on arrival,
    // wrap when looping, stop at the end otherwise.
    for (auto [e, route, agent] : registry.view<PatrolRoute, NavAgent>().each()) {
        if (route.points.empty()) continue;
        route.next = route.next % route.points.size();
        if (agent.arrived &&
            glm::distance(agent.target, route.points[route.next]) < 0.01F) {
            if (route.next + 1 < route.points.size()) {
                ++route.next;
            } else if (route.loop) {
                route.next = 0;
            } else {
                continue; // finished a one-way route
            }
            agent.arrived = false;
        }
        if (!agent.active && !agent.arrived) {
            agent.target = route.points[route.next];
            agent.active = true;
        }
    }

    const auto view = registry.view<Transform, NavAgent>();
    for (auto [e, transform, agent] : view.each()) {
        auto* controller = registry.try_get<CharacterController>(e);
        if (!agent.active) {
            if (controller != nullptr) controller->move = glm::vec3(0.0F);
            continue;
        }
        const glm::vec3 pos(transform.world[3]);

        // (Re)plan when the target moved or no plan exists.
        if (agent.path.empty() ||
            glm::distance(agent.target, agent.planned_target) > kRepathDistance) {
            auto path = navmesh.find_path(pos, agent.target);
            if (!path) {
                agent.active = false; // unreachable: stop rather than wander
                if (controller != nullptr) controller->move = glm::vec3(0.0F);
                continue;
            }
            agent.path = std::move(*path);
            agent.waypoint = 0;
            agent.planned_target = agent.target;
            agent.arrived = false;
        }

        // Advance waypoints as they are reached.
        while (agent.waypoint < agent.path.size() &&
               glm::distance(xz(pos), xz(agent.path[agent.waypoint])) <
                   (agent.waypoint + 1 == agent.path.size() ? kArriveReach : kWaypointReach)) {
            ++agent.waypoint;
        }
        if (agent.waypoint >= agent.path.size()) {
            agent.active = false;
            agent.arrived = true;
            agent.path.clear();
            if (controller != nullptr) controller->move = glm::vec3(0.0F);
            continue;
        }

        const glm::vec3 next = agent.path[agent.waypoint];
        glm::vec2 dir = xz(next) - xz(pos);
        const float len = glm::length(dir);
        if (len > 1e-5F) dir /= len;

        // Separation: ease away from nearby fellow agents (avoidance-lite; a
        // DetourCrowd upgrade slots in here later).
        for (auto [other, other_t, other_a] : view.each()) {
            if (other == e) continue;
            const glm::vec2 away = xz(pos) - xz(glm::vec3(other_t.world[3]));
            const float d = glm::length(away);
            if (d > 1e-4F && d < kSeparation) {
                dir += (away / d) * (1.0F - d / kSeparation);
            }
        }
        if (glm::length(dir) > 1e-5F) dir = glm::normalize(dir);

        if (controller != nullptr) {
            // The character system applies move * speed with collisions/gravity.
            controller->move = glm::vec3(dir.x, 0.0F, dir.y);
            controller->speed = agent.speed;
        } else {
            // Kinematic fallback: slide along the path, following its height.
            const float step = glm::min(agent.speed * dt, glm::max(len, 0.0F));
            glm::vec3 new_pos = pos + glm::vec3(dir.x, 0.0F, dir.y) * step;
            new_pos.y = glm::mix(pos.y, next.y, len > 1e-4F ? step / len : 1.0F);
            // Entities driven kinematically are treated as roots (like physics).
            transform.local[3] = glm::vec4(new_pos, 1.0F);
            transform.world[3] = glm::vec4(new_pos, 1.0F);
        }
    }
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error> run_nav_agent_self_test() {
    // The same walled plate the navmesh self-test uses.
    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> idx;
    const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        verts.insert(verts.end(), {a, b, c, d});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad({-20.0F, 0.0F, -20.0F}, {-20.0F, 0.0F, 20.0F}, {20.0F, 0.0F, 20.0F},
         {20.0F, 0.0F, -20.0F});
    const float x0 = -20.0F, x1 = 12.0F, z0 = -0.5F, z1 = 0.5F, h = 3.0F;
    quad({x0, h, z0}, {x0, h, z1}, {x1, h, z1}, {x1, h, z0});
    quad({x0, 0.0F, z0}, {x0, h, z0}, {x1, h, z0}, {x1, 0.0F, z0});
    quad({x1, 0.0F, z1}, {x1, h, z1}, {x0, h, z1}, {x0, 0.0F, z1});
    quad({x1, 0.0F, z0}, {x1, h, z0}, {x1, h, z1}, {x1, 0.0F, z1});

    auto navmesh = nav::NavMesh::build(verts, idx);
    if (!navmesh) return std::unexpected(navmesh.error());

    entt::registry registry;
    const entt::entity e = registry.create();
    auto& t = registry.emplace<Transform>(e);
    t.local = glm::translate(glm::mat4(1.0F), glm::vec3(-10.0F, 0.0F, -10.0F));
    t.world = t.local;
    auto& agent = registry.emplace<NavAgent>(e);
    agent.target = glm::vec3(-10.0F, 0.0F, 10.0F);
    agent.speed = 4.0F;
    agent.active = true;

    float max_x = -1e9F;
    for (int i = 0; i < 60 * 60 && !agent.arrived; ++i) {
        nav_agent_system(registry, *navmesh, 1.0F / 60.0F);
        max_x = glm::max(max_x, registry.get<Transform>(e).world[3].x);
    }
    if (!agent.arrived) {
        return std::unexpected(core::Error{"nav agent self-test: never arrived"});
    }
    const glm::vec3 final_pos(registry.get<Transform>(e).world[3]);
    if (glm::distance(glm::vec2(final_pos.x, final_pos.z), glm::vec2(-10.0F, 10.0F)) > 1.5F) {
        return std::unexpected(core::Error{"nav agent self-test: stopped short of the target"});
    }
    if (max_x < 10.0F) {
        return std::unexpected(
            core::Error{"nav agent self-test: walked through the wall instead of the gap"});
    }
    return {};
}

std::expected<void, core::Error> run_patrol_self_test() {
    // Flat plate, two-point looping route.
    std::vector<glm::vec3> verts{{-20.0F, 0.0F, -20.0F},
                                 {-20.0F, 0.0F, 20.0F},
                                 {20.0F, 0.0F, 20.0F},
                                 {20.0F, 0.0F, -20.0F}};
    std::vector<std::uint32_t> idx{0, 1, 2, 0, 2, 3};
    auto navmesh = nav::NavMesh::build(verts, idx);
    if (!navmesh) return std::unexpected(navmesh.error());

    entt::registry registry;
    const entt::entity e = registry.create();
    auto& t = registry.emplace<Transform>(e);
    t.local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F));
    t.world = t.local;
    auto& agent = registry.emplace<NavAgent>(e);
    agent.speed = 8.0F;
    auto& route = registry.emplace<PatrolRoute>(e);
    route.points = {{10.0F, 0.0F, 0.0F}, {-10.0F, 0.0F, 0.0F}};
    route.loop = true;

    bool reached_a = false;
    bool reached_b = false;
    bool wrapped = false;
    for (int i = 0; i < 60 * 40 && !(reached_a && reached_b && wrapped); ++i) {
        nav_agent_system(registry, *navmesh, 1.0F / 60.0F);
        const glm::vec3 pos(registry.get<Transform>(e).world[3]);
        reached_a = reached_a || glm::distance(pos, route.points[0]) < 1.0F;
        reached_b = reached_b || (reached_a && glm::distance(pos, route.points[1]) < 1.0F);
        wrapped = wrapped || (reached_b && route.next == 0);
    }
    if (!reached_a) return std::unexpected(core::Error{"patrol self-test: never reached A"});
    if (!reached_b) return std::unexpected(core::Error{"patrol self-test: never reached B"});
    if (!wrapped) return std::unexpected(core::Error{"patrol self-test: route did not loop"});
    return {};
}

} // namespace engine::scene
