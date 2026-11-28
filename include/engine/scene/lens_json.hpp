#pragma once
// JSON fields of an R20 physical lens, shared by the `camera` component codec
// (fields flattened into the component) and remote camera.get / camera.set
// (a nested `lens` object), so the two cannot disagree on names or ranges.

#include <nlohmann/json_fwd.hpp>

namespace engine::core {
class JsonFields;
}
namespace engine::renderer {
struct PhysicalCamera;
}

namespace engine::scene {

// Writes aperture_f, shutter_s, iso, sensor_height_mm, focal_length_mm and
// focus_distance_m into the object `out`.
void write_lens_fields(nlohmann::json& out, const renderer::PhysicalCamera& lens);

// Merges the lens fields present in `f` into `lens`, each checked against
// PhysicalCameraLimits; a focal length must be 0 (derived from the field of
// view) or at least min_focal_mm. Failures are reported through `f`.
void read_lens_fields(core::JsonFields& f, renderer::PhysicalCamera& lens);

} // namespace engine::scene
