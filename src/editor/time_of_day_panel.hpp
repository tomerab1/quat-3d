#pragma once

// Time of Day (ROADMAP R22.1): the editor UI of the scene's WorldClock —
// date and time-of-day scrubber, place (with presets), clock speed, and
// read-outs of the Sun, Moon and the day's events. Private editor header.

#include <entt/entity/fwd.hpp>

namespace engine::scene {
class Scene;
struct WorldClock;
} // namespace engine::scene

namespace engine::editor {

// The clock's rows, inside an open property grid (the Inspector's
// `world_clock` section and the Renderer panel's Time of Day section).
void draw_world_clock(entt::registry& registry, entt::entity entity, scene::WorldClock& clock);

// The Renderer panel's "Time of Day" section: the scene's clock, or a button
// that adds one.
void draw_time_of_day_section(scene::Scene* scene);

// True when the scene's clock currently drives its DirectionalLight (the
// Sun section then shows the light's angles read-only).
[[nodiscard]] bool clock_drives_sun(const entt::registry& registry);

// True for an entity that only holds the world clock (and the Transform
// every entity gets): it has no place in the world, so the Inspector hides
// its Transform section and the viewport draws no gizmo for it.
[[nodiscard]] bool clock_only_entity(const entt::registry& registry, entt::entity entity);

} // namespace engine::editor
