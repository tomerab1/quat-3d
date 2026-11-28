#pragma once

// The world clock's control surface (ROADMAP R22.1), shared by the remote
// time.* commands (src/remote/world/time_commands.cpp) and the Lua `time`
// table (src/script/script_api_services.cpp), so both validate and report
// alike. The reply shape is docs/remote-control.md "World clock".

#include <expected>

#include <entt/entity/fwd.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {

class Scene;

inline constexpr double k_max_clock_skip_hours = 87600.0; // ten years

// time.get: the clock's fields, `in_scene`, `entity`, `utc`, `julian_date`,
// `local_sidereal_time_deg`, and the Sun / Moon (positions, illuminance,
// phase) with the local day's events. Without a clock: the default clock,
// `enabled` false, `in_scene` false.
[[nodiscard]] nlohmann::json world_clock_report(const entt::registry& registry);

// time.set: the codec fields plus `local_time` ("hh:mm[:ss]", keeps the
// date), decoded strictly (unknown fields are errors) and applied all or
// nothing. Creates the clock when the scene has none (its full-sun intensity
// taken from the current light), and a sun light when the clock drives one
// and the scene has none; then applies the clock.
[[nodiscard]] std::expected<void, core::Error> set_world_clock(Scene& scene, const nlohmann::json& params);

// time.skip: moves the clock by `hours` (finite, |hours| <= 87600; wraps
// within the day under fixed_day) and applies it. Fails without a clock.
[[nodiscard]] std::expected<void, core::Error> skip_world_clock(entt::registry& registry, double hours);

} // namespace engine::scene
