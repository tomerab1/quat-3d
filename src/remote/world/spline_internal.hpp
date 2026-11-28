#pragma once

// Helpers shared by the spline.* world commands (spline_commands.cpp,
// spline_network_commands.cpp; ROADMAP G18).

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "engine/spline/curve.hpp"
#include "engine/spline/ground.hpp"
#include "internal.hpp"

namespace engine::remote::world {

// The world-space curve of `e`'s spline; not_found without a spline,
// invalid_params when it does not compile (the reason names the field).
// `field` names the entity parameter in errors.
[[nodiscard]] std::expected<spline::Curve, RpcError> spline_curve(const Context& ctx, entt::entity e,
                                                                  std::string_view field = "entity");

// {entity, spline, length, segments, closed, bounds}.
[[nodiscard]] Json spline_summary(const Context& ctx, entt::entity e);

// A 64-bit network / node / edge id as 16 hex digits (JSON numbers carry
// only 53 bits exactly).
[[nodiscard]] std::string id_string(std::uint64_t id);

void register_spline_network_commands(RpcServer& server, const ContextPtr& ctx);

// Terrain height in world space (null when the scene has no terrain).
[[nodiscard]] spline::GroundHeightFn ground_fn(const Context& ctx);

} // namespace engine::remote::world
