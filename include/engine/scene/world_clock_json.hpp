#pragma once

// JSON codec of the WorldClock (ROADMAP R22.1), shared by scene files (the
// `world_clock` component), scene.* remote commands and time.* commands:
//
//   {"enabled": true, "datetime": "2026-06-21T12:00:00+02:00",
//    "utc_offset_h": 2, "latitude_deg": 48.8566, "longitude_deg": 2.3522,
//    "north_offset_deg": 0, "time_scale": 1, "fixed_day": false,
//    "drive_sun": true, "sun_intensity": 3}
//
// `datetime` is ISO-8601 (astro::parse_iso8601). It is written in local time
// with its offset; read without a zone designator it means local time at the
// clock's utc_offset_h (the one given in the same object, else the current).
// A zone designator also sets the display offset unless the same object
// gives utc_offset_h.

#include <expected>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::core {
class JsonFields;
}

namespace engine::scene {

struct WorldClock;

[[nodiscard]] nlohmann::json encode_world_clock(const WorldClock& clock);

// Reads the codec fields present in `fields` into `clock` (merge: absent
// fields keep their values); problems are recorded on `fields`.
void decode_world_clock(core::JsonFields& fields, WorldClock& clock);

// Local date-time text of the clock ("…+02:00"), sub-second digits only when
// present.
[[nodiscard]] std::string world_clock_datetime(const WorldClock& clock);

} // namespace engine::scene
