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

struct NavAgent;

// Authoritative ground height at world (x, z), or nullopt where unknown (off
// the terrain). Supplied by the frame loop from the terrain heightmaps.
using GroundHeightFn = std::function<std::optional<float>(float x, float z)>;

// Send the agent to `target`: activates it and clears the previous walk's
// plan and arrived/failed state (the nav system plans on its next tick).
// Used by systems (patrol routes, behaviour trees).
void nav_agent_set_target(NavAgent& agent, const glm::vec3& target);

// A one-off order from the user (Ctrl+click, inspector Go, remote control):
// unpauses the agent and sends it to `target`; a patrol route resumes after.
void nav_agent_command(NavAgent& agent, const glm::vec3& target);

// The inspector's Stop: halts the agent where it stands and holds it there
// (patrol routes do not restart it) until nav_agent_resume.
void nav_agent_stop(NavAgent& agent);

// The inspector's Go: unpauses the agent. With a patrol route the route
// carries on; without one the agent walks to its current target.
void nav_agent_resume(entt::registry& registry, entt::entity entity);

// World position of the agent's feet: the entity origin minus ground_offset,
// or the bottom of its CharacterController capsule.
[[nodiscard]] glm::vec3 nav_agent_feet(const entt::registry& registry, entt::entity entity);

// Advance every active NavAgent: repath on the navmesh when the target moved,
// steer toward the current waypoint with simple agent-agent separation, and
// either drive the entity's CharacterController (move/speed) or translate the
// Transform kinematically. Call before character_system each tick.
//
// Detour returns best-effort paths (partial when the buffer fills or the target
// poly is unreachable): when an agent exhausts a partial path it replans the
// next leg from there, and when no further progress is possible it gives up
// (failed). A target off the mesh within NavMesh::query_extents snaps to its
// nearest polygon (arriving there counts). Failed plans retry on a cooldown
// and give up after a few attempts; a PatrolRoute skips points its agent
// failed to reach.
//
// `ground_height(x, z)` (optional) supplies the authoritative ground height for
// kinematic agents — coarse terrain navmeshes can float metres off the real
// surface, so path heights are only a fallback. Agents with a
// CharacterController ignore it (physics owns their height).
void nav_agent_system(entt::registry& registry, const nav::NavMesh& navmesh, float dt,
                      const GroundHeightFn& ground_height = {});

} // namespace engine::scene
