// Component JSON codec: camera_rig and camera_rig_input (G15.1).

#include "component_json_camera_rig.hpp"

#include <cmath>
#include <string>

namespace engine::scene::detail {

namespace {

using json = nlohmann::json;
using core::JsonFields;

constexpr core::EnumName<RigBounds::Shape> k_bound_shapes[] = {{"none", RigBounds::Shape::none},
                                                               {"rect", RigBounds::Shape::rect},
                                                               {"circle", RigBounds::Shape::circle}};
constexpr float k_max_coord = 1.0e6F;

[[nodiscard]] json vec3_json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
[[nodiscard]] json vec2_json(const glm::vec2& v) { return json::array({v.x, v.y}); }

void read_vec2(JsonFields& f, std::string_view key, glm::vec2& out) {
    const json* v = f.raw(key);
    if (v == nullptr || v->is_null()) return;
    glm::vec2 r{};
    if (!v->is_array() || v->size() != 2 || !JsonFields::read_float((*v)[0], r.x) ||
        !JsonFields::read_float((*v)[1], r.y) || std::abs(r.x) > k_max_coord || std::abs(r.y) > k_max_coord) {
        f.fail(key, "expected [x, z] in metres");
        return;
    }
    out = r;
}

void decode_bounds(JsonFields& parent, RigBounds& b) {
    const json* v = parent.raw("bounds");
    if (v == nullptr || v->is_null()) return;
    JsonFields f(*v, "camera_rig.bounds", parent.strict());
    f.enumeration("shape", b.shape, k_bound_shapes);
    read_vec2(f, "min", b.min);
    read_vec2(f, "max", b.max);
    read_vec2(f, "center", b.center);
    f.number("radius", b.radius, 0.0F, k_max_coord).number("margin", b.margin, 0.0F, 1.0e4F);
    if (auto ok = f.finish(); !ok) parent.fail("bounds", ok.error().message);
}

[[nodiscard]] std::string shape_name(RigBounds::Shape shape) {
    for (const auto& n : k_bound_shapes) {
        if (n.value == shape) return std::string(n.name);
    }
    return "none";
}

void read_action_name(JsonFields& f, std::string_view key, std::string& out) {
    f.string(key, out, 64);
}

} // namespace

json encode_camera_rig(const CameraRig& r) {
    const RigBounds& b = r.bounds;
    json out = {{"focus", vec3_json(r.focus)},
                {"yaw_deg", r.yaw_deg},
                {"pitch_deg", r.pitch_deg},
                {"distance", r.distance},
                {"min_distance", r.min_distance},
                {"max_distance", r.max_distance},
                {"pitch_near_deg", r.pitch_near_deg},
                {"pitch_far_deg", r.pitch_far_deg},
                {"pitch_follows_zoom", r.pitch_follows_zoom},
                {"min_pitch_deg", r.min_pitch_deg},
                {"max_pitch_deg", r.max_pitch_deg},
                {"yaw_step_deg", r.yaw_step_deg},
                {"focus_halflife_s", r.focus_halflife_s},
                {"yaw_halflife_s", r.yaw_halflife_s},
                {"zoom_halflife_s", r.zoom_halflife_s},
                {"pitch_halflife_s", r.pitch_halflife_s},
                {"ground_clearance", r.ground_clearance},
                {"focus_ground_halflife_s", r.focus_ground_halflife_s},
                {"follow_ground", r.follow_ground},
                {"drag_inertia_halflife_s", r.drag_inertia_halflife_s},
                {"bounds", {{"shape", shape_name(b.shape)},
                            {"min", vec2_json(b.min)},
                            {"max", vec2_json(b.max)},
                            {"center", vec2_json(b.center)},
                            {"radius", b.radius},
                            {"margin", b.margin}}}};
    // Read-only: where the camera is now (the damped state chasing the targets).
    if (r.state.valid) {
        out["current"] = {{"focus", vec3_json(r.state.focus)},
                          {"yaw_deg", r.state.yaw_deg},
                          {"pitch_deg", r.state.pitch_deg},
                          {"distance", std::exp(r.state.log_distance)},
                          {"eye", vec3_json(rig_eye(r.state.focus, r.state.yaw_deg, r.state.pitch_deg,
                                                    std::exp(r.state.log_distance)))}};
    }
    return out;
}

void decode_camera_rig(JsonFields& f, CameraRig& r) {
    (void)f.raw("current"); // read-only, written back by scene files: ignored
    glm::vec3 focus = r.focus;
    f.vec3("focus", focus);
    if (glm::any(glm::greaterThan(glm::abs(focus), glm::vec3(k_max_coord)))) {
        f.fail("focus", "expected coordinates within 1e6 metres");
    } else {
        r.focus = focus;
    }
    f.number("yaw_deg", r.yaw_deg, -3600.0F, 3600.0F);
    f.number("pitch_deg", r.pitch_deg, -89.0F, 89.0F);
    f.number("distance", r.distance, 0.01F, 1.0e5F);
    f.number("min_distance", r.min_distance, 0.01F, 1.0e5F);
    f.number("max_distance", r.max_distance, 0.01F, 1.0e5F);
    f.number("pitch_near_deg", r.pitch_near_deg, -89.0F, 89.0F);
    f.number("pitch_far_deg", r.pitch_far_deg, -89.0F, 89.0F);
    f.boolean("pitch_follows_zoom", r.pitch_follows_zoom);
    f.number("min_pitch_deg", r.min_pitch_deg, -89.0F, 89.0F);
    f.number("max_pitch_deg", r.max_pitch_deg, -89.0F, 89.0F);
    f.number("yaw_step_deg", r.yaw_step_deg, 0.0F, 180.0F);
    if (r.yaw_step_deg > 0.0F) {
        const float turns = 360.0F / r.yaw_step_deg;
        if (std::abs(turns - std::round(turns)) > 1.0e-3F) {
            f.fail("yaw_step_deg", "must divide 360 (e.g. 15, 30, 45, 90), or be 0 for free rotation");
        }
    }
    f.number("focus_halflife_s", r.focus_halflife_s, 0.0F, 10.0F);
    f.number("yaw_halflife_s", r.yaw_halflife_s, 0.0F, 10.0F);
    f.number("zoom_halflife_s", r.zoom_halflife_s, 0.0F, 10.0F);
    f.number("pitch_halflife_s", r.pitch_halflife_s, 0.0F, 10.0F);
    f.number("ground_clearance", r.ground_clearance, 0.0F, 1000.0F);
    f.number("focus_ground_halflife_s", r.focus_ground_halflife_s, 0.0F, 10.0F);
    f.boolean("follow_ground", r.follow_ground);
    f.number("drag_inertia_halflife_s", r.drag_inertia_halflife_s, 0.0F, 10.0F);
    decode_bounds(f, r.bounds);
    if (r.max_distance < r.min_distance) f.fail("max_distance", "must be >= min_distance");
    if (r.max_pitch_deg < r.min_pitch_deg) f.fail("max_pitch_deg", "must be >= min_pitch_deg");
}

json encode_camera_rig_input(const CameraRigInput& in) {
    return {{"pan", in.pan},
            {"orbit", in.orbit},
            {"orbit_delta", in.orbit_delta},
            {"pitch_delta", in.pitch_delta},
            {"zoom", in.zoom},
            {"drag_pan", in.drag_pan},
            {"pan_speed", in.pan_speed},
            {"orbit_speed_deg", in.orbit_speed_deg},
            {"orbit_delta_deg", in.orbit_delta_deg},
            {"zoom_step", in.zoom_step},
            {"zoom_to_cursor", in.zoom_to_cursor}};
}

void decode_camera_rig_input(JsonFields& f, CameraRigInput& in) {
    read_action_name(f, "pan", in.pan);
    read_action_name(f, "orbit", in.orbit);
    read_action_name(f, "orbit_delta", in.orbit_delta);
    read_action_name(f, "pitch_delta", in.pitch_delta);
    read_action_name(f, "zoom", in.zoom);
    read_action_name(f, "drag_pan", in.drag_pan);
    f.number("pan_speed", in.pan_speed, 0.0F, 100.0F);
    f.number("orbit_speed_deg", in.orbit_speed_deg, 0.0F, 3600.0F);
    f.number("orbit_delta_deg", in.orbit_delta_deg, -100.0F, 100.0F);
    f.number("zoom_step", in.zoom_step, 1.001F, 4.0F);
    f.boolean("zoom_to_cursor", in.zoom_to_cursor);
}

} // namespace engine::scene::detail
