// NavAgent path following + separation steering (Phase 13, Slice 13.2).

#include <cmath>
#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/nav/navmesh.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/navigation.hpp"

namespace engine::scene {

namespace {

constexpr float kWaypointReach = 0.6F;     // advance when this close (xz)
constexpr float kArriveReach = 0.4F;       // final waypoint tolerance
constexpr float kRepathDistance = 0.5F;    // target moved farther -> repath
constexpr float kSeparation = 1.2F;        // agent-agent comfort radius
constexpr float kPlanRetrySeconds = 1.0F;  // cooldown after a failed plan
constexpr std::uint8_t kMaxPlanFailures = 3; // attempts without progress before giving up

[[nodiscard]] glm::vec2 xz(const glm::vec3& v) { return {v.x, v.z}; }

void stop(CharacterController* controller) {
    if (controller != nullptr) controller->move = glm::vec3(0.0F);
}

// The walk is over without reaching the target.
void give_up(NavAgent& agent) {
    agent.active = false;
    agent.arrived = false;
    agent.failed = true;
    agent.path.clear();
    agent.retry_s = 0.0F;
}

// An attempt that got nowhere: retry after a cooldown, give up after a few.
void attempt_failed(NavAgent& agent) {
    agent.path.clear();
    if (++agent.plan_failures >= kMaxPlanFailures) {
        give_up(agent);
    } else {
        agent.retry_s = kPlanRetrySeconds;
    }
}

// Patrol routes feed their idle agent point by point: once the agent arrived
// at (or gave up on) the current point the route advances, wrapping when
// looping and finishing at the end otherwise. Whenever the agent is idle and
// not paused — after a commanded move, a failed walk, a late first point —
// the route (re)sends it to the current point.
void feed_patrol_routes(entt::registry& registry) {
    for (auto [e, route, agent] : registry.view<PatrolRoute, NavAgent>().each()) {
        if (route.points.empty() || agent.paused || agent.active) continue;
        route.next = route.next % route.points.size();
        const bool at_point = glm::distance(agent.target, route.points[route.next]) < 0.01F;
        const bool done_here = !agent.commanded && at_point && (agent.arrived || agent.failed);
        if (done_here || route.finished) {
            if (route.next + 1 < route.points.size()) {
                ++route.next;
            } else if (route.loop) {
                route.next = 0;
            } else {
                route.finished = true; // one-way route done (appending points resumes)
                continue;
            }
            route.finished = false;
            // A one-point loop stays put instead of replanning every frame.
            if (done_here && glm::distance(agent.target, route.points[route.next]) < 0.01F) {
                continue;
            }
        }
        nav_agent_set_target(agent, route.points[route.next]);
    }
}

} // namespace

void nav_agent_set_target(NavAgent& agent, const glm::vec3& target) {
    agent.target = target;
    agent.active = true;
    agent.commanded = false;
    agent.arrived = false;
    agent.failed = false;
    agent.plan_failures = 0;
    agent.path_partial = false;
    agent.path.clear();
    agent.waypoint = 0;
    agent.retry_s = 0.0F;
}

void nav_agent_command(NavAgent& agent, const glm::vec3& target) {
    nav_agent_set_target(agent, target);
    agent.commanded = true;
    agent.paused = false;
}

void nav_agent_stop(NavAgent& agent) {
    agent.paused = true;
    agent.active = false;
    agent.arrived = false;
    agent.failed = false;
    agent.path.clear();
    agent.retry_s = 0.0F;
}

void nav_agent_resume(entt::registry& registry, entt::entity entity) {
    auto* agent = registry.try_get<NavAgent>(entity);
    if (agent == nullptr) return;
    agent->paused = false;
    const auto* route = registry.try_get<PatrolRoute>(entity);
    if (route == nullptr || route->points.empty()) {
        nav_agent_command(*agent, agent->target);
    }
    // With a route, the patrol feed sends the agent on next tick.
}

glm::vec3 nav_agent_feet(const entt::registry& registry, entt::entity entity) {
    const auto* t = registry.try_get<Transform>(entity);
    if (t == nullptr) return glm::vec3(0.0F);
    glm::vec3 pos(t->world[3]);
    if (const auto* cc = registry.try_get<CharacterController>(entity)) {
        pos.y -= cc->half_height + cc->radius;
    } else if (const auto* agent = registry.try_get<NavAgent>(entity)) {
        pos.y -= agent->ground_offset;
    }
    return pos;
}

void nav_agent_system(entt::registry& registry, const nav::NavMesh& navmesh, float dt,
                      const GroundHeightFn& ground_height) {
    if (!navmesh.valid()) return;

    feed_patrol_routes(registry);

    const auto view = registry.view<Transform, NavAgent>();
    for (auto [e, transform, agent] : view.each()) {
        auto* controller = registry.try_get<CharacterController>(e);
        if (!agent.active) {
            stop(controller);
            continue;
        }
        // Failed plans retry on a cooldown instead of hammering the query
        // (and, through the patrol feed, replanning every frame).
        if (agent.retry_s > 0.0F) {
            agent.retry_s -= dt;
            stop(controller);
            continue;
        }

        const glm::vec3 pos(transform.world[3]);
        const glm::vec3 feet = nav_agent_feet(registry, e);

        // (Re)plan when the target moved or no plan exists.
        if (agent.path.empty() ||
            glm::distance(agent.target, agent.planned_target) > kRepathDistance) {
            auto plan = navmesh.plan_path(feet, agent.target);
            if (!plan) { // no polygon near the agent or the target
                attempt_failed(agent);
                stop(controller);
                continue;
            }
            // A partial plan that cannot take the agent anywhere: the target
            // is unreachable from here (a disconnected island, or the agent
            // already stands at the closest reachable point).
            if (plan->partial && glm::distance(xz(plan->points.back()), xz(feet)) < kArriveReach) {
                give_up(agent);
                stop(controller);
                continue;
            }
            agent.path = std::move(plan->points);
            agent.path_partial = plan->partial;
            agent.waypoint = 0;
            agent.planned_target = agent.target;
            agent.arrived = false;
        }

        // Advance waypoints as they are reached.
        while (agent.waypoint < agent.path.size() &&
               glm::distance(xz(pos), xz(agent.path[agent.waypoint])) <
                   (agent.waypoint + 1 == agent.path.size() ? kArriveReach : kWaypointReach)) {
            ++agent.waypoint;
            agent.plan_failures = 0; // progress
        }
        if (agent.waypoint >= agent.path.size()) {
            agent.path.clear();
            if (!agent.path_partial) {
                // The end of a complete path: the target, or the closest
                // on-mesh point to it.
                agent.active = false;
                agent.arrived = true;
                stop(controller);
            }
            // Otherwise the (partial) path ran out short of the target: leave
            // the agent active with no path so the next frame plans the next
            // leg from here (or gives up when there is none).
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
            // Kinematic fallback: slide along the path. Heights come from the
            // real ground when a sampler is provided — coarse terrain bakes
            // put the navmesh metres off the surface — falling back to the
            // path's own (navmesh) heights.
            const float step = glm::min(agent.speed * dt, glm::max(len, 0.0F));
            glm::vec3 new_pos = pos + glm::vec3(dir.x, 0.0F, dir.y) * step;
            std::optional<float> ground;
            if (ground_height) ground = ground_height(new_pos.x, new_pos.z);
            const float ground_y =
                ground ? *ground
                       : glm::mix(pos.y - agent.ground_offset, next.y,
                                  len > 1e-4F ? step / len : 1.0F);
            new_pos.y = ground_y + agent.ground_offset;
            // Entities driven kinematically are treated as roots (like physics).
            transform.local[3] = glm::vec4(new_pos, 1.0F);
            transform.world[3] = glm::vec4(new_pos, 1.0F);
        }
    }
}

} // namespace engine::scene
