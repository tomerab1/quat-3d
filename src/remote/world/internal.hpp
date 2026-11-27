#pragma once

// Private helpers shared by the world-command translation units
// (src/remote/world/*.cpp). See include/engine/remote/world_commands.hpp.

#include <memory>
#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

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
void register_camera_commands(RpcServer& server, const ContextPtr& ctx);
void register_app_commands(RpcServer& server, const ContextPtr& ctx);

} // namespace engine::remote::world
