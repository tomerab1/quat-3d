#pragma once

// Seasons (ROADMAP R25.1): the Inspector sections of the scene's
// SeasonState (mode, phase, accelerated year, weather sequence, read-outs of
// the season weights, day length and the climate now) and of its Climate
// (curve plots over the year, the sequence's scalars). Private editor header.

#include <entt/entity/fwd.hpp>

namespace engine::scene {
struct SeasonState;
}
namespace engine::weather {
struct Climate;
}

namespace engine::editor {

// Rows inside an open property grid.
void draw_season(entt::registry& registry, entt::entity entity, scene::SeasonState& season);
void draw_climate(entt::registry& registry, entt::entity entity, weather::Climate& climate);

} // namespace engine::editor
