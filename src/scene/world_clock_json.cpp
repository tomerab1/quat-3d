// WorldClock JSON codec — see include/engine/scene/world_clock_json.hpp.

#include "engine/scene/world_clock_json.hpp"

#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/astro/time.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/scene/world_clock.hpp"

namespace engine::scene {

namespace {

constexpr std::size_t k_max_datetime_length = 64;

} // namespace

std::string world_clock_datetime(const WorldClock& c) {
    const bool whole = c.utc.ns % astro::k_ns_per_second == 0;
    std::string s = astro::format_iso8601(c.utc, c.utc_offset_min, whole ? 0 : 9);
    if (!whole) {
        // Trim trailing zeros of the fraction (before the zone designator).
        const std::size_t dot = s.find('.');
        std::size_t       zone = s.find_first_of("Z+-", dot);
        std::size_t       end = zone;
        while (end > dot + 2 && s[end - 1] == '0') --end;
        s.erase(end, zone - end);
    }
    return s;
}

nlohmann::json encode_world_clock(const WorldClock& c) {
    return {{"enabled", c.enabled},
            {"datetime", world_clock_datetime(c)},
            {"utc_offset_h", c.utc_offset_min / 60.0},
            {"latitude_deg", c.latitude_deg},
            {"longitude_deg", c.longitude_deg},
            {"north_offset_deg", c.north_offset_deg},
            {"time_scale", c.time_scale},
            {"fixed_day", c.fixed_day},
            {"drive_sun", c.drive_sun},
            {"sun_intensity", c.sun_intensity}};
}

void decode_world_clock(core::JsonFields& f, WorldClock& c) {
    f.boolean("enabled", c.enabled);
    const bool has_offset = f.has("utc_offset_h");
    float      hours = static_cast<float>(c.utc_offset_min) / 60.0F;
    f.number("utc_offset_h", hours, -14.0F, 14.0F);
    if (has_offset && f.ok()) c.utc_offset_min = static_cast<std::int32_t>(std::lround(hours * 60.0F));

    const bool  has_datetime = f.has("datetime");
    std::string text;
    f.string("datetime", text, k_max_datetime_length);
    if (has_datetime && f.ok()) {
        if (auto parsed = astro::parse_iso8601(text); !parsed) {
            f.fail("datetime", parsed.error());
        } else {
            // A zone designator also becomes the display zone, unless the
            // same object sets utc_offset_h.
            const int            zone = parsed->utc_offset_min.value_or(c.utc_offset_min);
            const astro::UtcTime t = astro::utc_from_civil(parsed->civil, zone);
            // The local year was checked; the offset can still carry the
            // instant past the supported UTC range (the clock would clamp it
            // on its next tick and the sun would jump).
            if (!astro::in_supported_range(t)) {
                f.fail("datetime", "the UTC instant " + astro::format_iso8601(t, 0) +
                                       " is outside the supported years " +
                                       std::to_string(astro::k_min_year) + ".." +
                                       std::to_string(astro::k_max_year));
            } else {
                if (parsed->utc_offset_min && !has_offset) c.utc_offset_min = *parsed->utc_offset_min;
                c.utc = t;
            }
        }
    }
    f.number("latitude_deg", c.latitude_deg, -90.0, 90.0);
    f.number("longitude_deg", c.longitude_deg, -180.0, 180.0);
    f.number("north_offset_deg", c.north_offset_deg, -360.0, 360.0);
    f.number("time_scale", c.time_scale, 0.0, k_max_clock_time_scale);
    f.boolean("fixed_day", c.fixed_day);
    f.boolean("drive_sun", c.drive_sun);
    f.number("sun_intensity", c.sun_intensity, 0.0F, 1.0e5F);
}

} // namespace engine::scene
