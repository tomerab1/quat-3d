#pragma once

// Navigation systems and helpers (Phase 13): NavAgent path following and
// patrol routes.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::nav {
class NavMesh;
}

namespace engine::scene {

// Authoritative ground height at world (x, z), or nullopt where unknown (off
// the terrain). Supplied by the frame loop from the terrain heightmaps.
using GroundHeightFn = std::function<std::optional<float>(float x, float z)>;

// Advance every active NavAgent: repath on the navmesh when the target moved,
// steer toward the current waypoint with simple agent-agent separation, and
// either drive the entity's CharacterController (move/speed) or translate the
// Transform kinematically. Call before character_system each tick.
//
// Detour returns best-effort paths (partial when the buffer fills or the target
// poly is unreachable): when an agent exhausts its path away from the target it
// replans the next leg, and when it can make no further progress it settles for
// the closest reachable point (arrived). Failed plans retry on a cooldown; a
// PatrolRoute skips unreachable points instead of stalling the route.
//
// `ground_height(x, z)` (optional) supplies the authoritative ground height for
// kinematic agents — coarse terrain navmeshes can float metres off the real
// surface, so path heights are only a fallback. Agents with a
// CharacterController ignore it (physics owns their height).
void nav_agent_system(entt::registry& registry, const nav::NavMesh& navmesh, float dt,
                      const GroundHeightFn& ground_height = {});

// A kinematic agent walks from one side of the walled test plate to the other:
// it must arrive at the target and must have detoured through the gap.
[[nodiscard]] std::expected<void, core::Error> run_nav_agent_self_test();

// A looping two-point patrol drives the agent A -> B -> A on a flat plate.
[[nodiscard]] std::expected<void, core::Error> run_patrol_self_test();

} // namespace engine::scene
