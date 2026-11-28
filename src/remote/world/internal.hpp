#pragma once

// Private helpers shared by the world-command translation units
// (src/remote/world/*.cpp). See include/engine/remote/world_commands.hpp.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/asset/mesh_asset.hpp" // Aabb
#include "engine/core/json_fields.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/remote/world_commands.hpp"

namespace engine::scene {
class Scene;
}

namespace engine::remote::world {

// State every handler shares. Captured by shared_ptr so the handlers own it.
struct Context {
    RpcServer*     server = nullptr;
    scene::Scene*  scene = nullptr;
    WorldServices  services;
    // debug.step requests parked in services.when_stepped (capped).
    std::size_t    step_waits = 0;
};
using ContextPtr = std::shared_ptr<Context>;

// ---- Errors ----------------------------------------------------------------

[[nodiscard]] inline std::unexpected<RpcError> bad_params(std::string message) {
    return rpc_fail(rpc_code::invalid_params, std::move(message));
}
// `what` names the capability, e.g. "the view camera".
[[nodiscard]] std::unexpected<RpcError> unavailable(std::string_view what);
// A JsonFields error (or other decode error) as invalid_params.
[[nodiscard]] std::expected<void, RpcError> finish(const core::JsonFields& fields);

// ---- Entity references -------------------------------------------------------

// The integer id clients see (entt's full identifier, version included, so a
// stale id never aliases a recycled entity).
[[nodiscard]] Json entity_json(entt::entity e);

// Resolves `ref` (integer id or exact unique name) to a live entity.
// `field` names the parameter in error messages.
[[nodiscard]] std::expected<entt::entity, RpcError>
resolve_entity(const entt::registry& registry, const Json& ref, std::string_view field);

// As resolve_entity, but JSON null means "no parent" (returns entt::null).
[[nodiscard]] std::expected<entt::entity, RpcError>
resolve_parent(const entt::registry& registry, const Json& ref, std::string_view field);

// Reads a required entity reference field from `fields`.
[[nodiscard]] std::expected<entt::entity, RpcError>
required_entity(core::JsonFields& fields, const entt::registry& registry, std::string_view key);

// ---- Hierarchy / transforms ----------------------------------------------------

[[nodiscard]] entt::entity parent_of(const entt::registry& registry, entt::entity e);

// World matrix composed from the local transforms up the parent chain — exact
// even before the next TransformSystem tick.
[[nodiscard]] glm::mat4 world_matrix(const entt::registry& registry, entt::entity e);

// Recomputes Transform::world for `e` and its descendants, so reads in the
// same frame (scene.get, camera.frame) see edits immediately.
void refresh_world(entt::registry& registry, entt::entity e);

// `e` and all of its descendants (pre-order; cycle-safe).
[[nodiscard]] std::vector<entt::entity> subtree(const entt::registry& registry, entt::entity e);

// ---- Components ------------------------------------------------------------------

// Validates `components` (an object of codec components) against a scratch copy
// of `existing` (or a fresh entity when entt::null) without touching the
// scene, then applies it to `target` in the same order. The dry run makes
// create/update atomic: a bad field anywhere leaves the entity unchanged.
[[nodiscard]] std::expected<void, RpcError>
apply_components_atomic(entt::registry& registry, entt::entity target, const Json& components);

// ---- Terrain -----------------------------------------------------------------------

// The terrain the runtime drives: the first entity with a Terrain component
// (the one the frame loop generates), anchored at its current world position.
struct TerrainRef {
    entt::entity entity = entt::null;
    glm::vec3    anchor{0.0F};
    bool         streaming = false;
    // A shape edit not yet picked up by the runtime: the loaded tiles are
    // stale until it regenerates them.
    bool         regenerating = false;
};
[[nodiscard]] std::optional<TerrainRef> find_terrain(const entt::registry& registry);

// World-space terrain height at (x, z), nullopt where no tile is loaded, or
// while the terrain regenerates (a pending shape edit, or a single tile whose
// generation job is running). Needs services.terrain_height.
[[nodiscard]] std::optional<float> terrain_height_at(const Context& ctx, const TerrainRef& terrain,
                                                     float x, float z);

// ---- Bounds and surfaces (issue #40) ----------------------------------------------

// World-space bounds of what `e` and its descendants draw, through exact world
// matrices (world_matrix). With `include_terrain`, the loaded terrain tiles
// count for the Terrain entity. nullopt when nothing draws.
[[nodiscard]] std::optional<asset::Aabb> subtree_world_bounds(const Context& ctx, entt::entity e,
                                                              bool include_terrain);
// The same drawables in `e`'s own frame (its model space; terrain excluded).
[[nodiscard]] std::optional<asset::Aabb> subtree_local_bounds(const Context& ctx, entt::entity e);
// {min, max}, or null.
[[nodiscard]] Json bounds_json(const std::optional<asset::Aabb>& box);

// A surface found below a point: its height and normal, what it is
// ("terrain", "collider", "body", "character", "static_world") and the
// entity it belongs to (entt::null for ownerless physics bodies).
struct Surface {
    float        y = 0.0F;
    glm::vec3    normal{0.0F, 1.0F, 0.0F};
    const char*  kind = "";
    entt::entity entity = entt::null;
};
// What lies below (x, z): the ground (terrain, ownerless static bodies) and
// the highest object surface (other entities' colliders, physics bodies)
// whose top is below `top_y`. Entities in `ignore` (the one being placed)
// are skipped. surface_query.cpp has the rules.
struct SurfaceSample {
    std::optional<Surface> ground;
    std::optional<Surface> object;
};
[[nodiscard]] SurfaceSample sample_surfaces(const Context& ctx,
                                            const std::optional<TerrainRef>& terrain,
                                            const std::unordered_set<entt::entity>& ignore,
                                            float x, float z, float top_y);

// ---- Misc ----------------------------------------------------------------------

[[nodiscard]] inline Json vec3_json(const glm::vec3& v) { return Json::array({v.x, v.y, v.z}); }

// Path parameter `key` (required) confined to the project root: returns the
// absolute path and writes the normalized project-relative form to `relative`.
[[nodiscard]] std::expected<std::filesystem::path, RpcError>
project_path(const Context& ctx, core::JsonFields& fields, std::string_view key,
             std::string& relative);

// ---- Registration (one per domain file) -----------------------------------------

void register_scene_commands(RpcServer& server, const ContextPtr& ctx);
void register_batch_command(RpcServer& server, const ContextPtr& ctx);
void register_environment_commands(RpcServer& server, const ContextPtr& ctx);
void register_terrain_commands(RpcServer& server, const ContextPtr& ctx);
void register_placement_commands(RpcServer& server, const ContextPtr& ctx);
void register_camera_commands(RpcServer& server, const ContextPtr& ctx);
void register_app_commands(RpcServer& server, const ContextPtr& ctx);
void register_debug_commands(RpcServer& server, const ContextPtr& ctx);
void register_debug_query_commands(RpcServer& server, const ContextPtr& ctx);
void register_tuning_commands(RpcServer& server, const ContextPtr& ctx);
void register_behavior_commands(RpcServer& server, const ContextPtr& ctx);
void register_behavior_edit_commands(RpcServer& server, const ContextPtr& ctx);
void register_script_commands(RpcServer& server, const ContextPtr& ctx);
void register_shader_commands(RpcServer& server, const ContextPtr& ctx);
void register_time_commands(RpcServer& server, const ContextPtr& ctx);

// {paused, time_scale, pending_steps, steps_run, sim_frames, sim_time_s}.
[[nodiscard]] Json sim_clock_json(const core::SimClock& clock);

} // namespace engine::remote::world
