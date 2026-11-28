#include "engine/scene/lens_json.hpp"

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/renderer/physical_camera.hpp"

namespace engine::scene {

void write_lens_fields(nlohmann::json& out, const renderer::PhysicalCamera& lens) {
    out["aperture_f"] = lens.aperture_f;
    out["shutter_s"] = lens.shutter_s;
    out["iso"] = lens.iso;
    out["sensor_height_mm"] = lens.sensor_height_mm;
    out["focal_length_mm"] = lens.focal_length_mm;
    out["focus_distance_m"] = lens.focus_distance_m;
}

void read_lens_fields(core::JsonFields& f, renderer::PhysicalCamera& lens) {
    using L = renderer::PhysicalCameraLimits;
    f.number("aperture_f", lens.aperture_f, L::min_aperture, L::max_aperture)
        .number("shutter_s", lens.shutter_s, L::min_shutter, L::max_shutter)
        .number("iso", lens.iso, L::min_iso, L::max_iso)
        .number("sensor_height_mm", lens.sensor_height_mm, L::min_sensor_mm, L::max_sensor_mm)
        .number("focal_length_mm", lens.focal_length_mm, 0.0F, L::max_focal_mm)
        .number("focus_distance_m", lens.focus_distance_m, L::min_focus_m, L::max_focus_m);
    // A near-zero focal length gives a field of view of almost 180 degrees
    // and a degenerate projection.
    if (lens.focal_length_mm > 0.0F && lens.focal_length_mm < L::min_focal_mm) {
        f.fail("focal_length_mm", "must be 0 (derive from the field of view) or at least 1 mm");
    }
}

} // namespace engine::scene
