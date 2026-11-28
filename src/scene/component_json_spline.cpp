// Component JSON codec: spline (ROADMAP G18.1).
//
//   "spline": {"kind": "centripetal_catmull_rom" | "cubic_bezier" | "polyline",
//              "closed": false, "follow_ground": true, "ground_offset": 0,
//              "network": 0, "id": 0,
//              "points": [[x, y, z], {"position": [x, y, z], "width": 2, "height": 1, "tags": 4}, ...]}
//
// A point with the default width (1), height (0) and tags (0) is written as
// a bare [x, y, z]; both forms are read.

#include "component_json_spline.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace engine::scene::detail {

namespace {

using json = nlohmann::json;
using core::JsonFields;

constexpr core::EnumName<SplineKind> k_kinds[] = {
    {"centripetal_catmull_rom", SplineKind::centripetal_catmull_rom},
    {"cubic_bezier", SplineKind::cubic_bezier},
    {"polyline", SplineKind::polyline}};

[[nodiscard]] bool read_position(const json& v, glm::vec3& out) {
    if (!v.is_array() || v.size() != 3) return false;
    glm::vec3 p{};
    for (int i = 0; i < 3; ++i) {
        if (!JsonFields::read_float(v[static_cast<std::size_t>(i)], p[i]) ||
            std::abs(p[i]) > spline::k_max_coordinate) {
            return false;
        }
    }
    out = p;
    return true;
}

// Records "a.b[2].c: why" as the field a.b[2].c failing with `why`, so the
// error path reads spline.points[2].width: ...
void fail_with_path(JsonFields& f, const std::string& message) {
    const auto colon = message.find(": ");
    if (colon == std::string::npos) {
        f.fail("points", message);
    } else {
        f.fail(message.substr(0, colon), message.substr(colon + 2));
    }
}

} // namespace

json encode_spline_point(const SplinePoint& p) {
    json pos = json::array({p.position.x, p.position.y, p.position.z});
    if (p.width == 1.0F && p.height == 0.0F && p.tags == 0) return pos;
    return {{"position", std::move(pos)}, {"width", p.width}, {"height", p.height}, {"tags", p.tags}};
}

bool decode_spline_point(const json& v, std::string context, bool strict, SplinePoint& out, std::string& error) {
    constexpr const char* k_position_error = "expected [x, y, z] in metres (finite, within 1e6)";
    SplinePoint p;
    if (v.is_array()) {
        if (!read_position(v, p.position)) {
            error = context + ": " + k_position_error;
            return false;
        }
        out = p;
        return true;
    }
    if (!v.is_object()) {
        error = context + ": expected [x, y, z] or {position, width, height, tags}";
        return false;
    }
    JsonFields f(v, context, strict);
    if (const json* pos = f.raw("position"); pos == nullptr) {
        f.fail("position", "required");
    } else if (!read_position(*pos, p.position)) {
        f.fail("position", k_position_error);
    }
    f.number("width", p.width, 0.0F, spline::k_max_width)
        .number("height", p.height, -spline::k_max_width, spline::k_max_width)
        .integer("tags", p.tags, 0, 0xFFFFFFFFLL);
    if (auto ok = f.finish(); !ok) {
        error = ok.error().message;
        return false;
    }
    out = p;
    return true;
}

json encode_spline(const Spline& s) {
    json points = json::array();
    for (const SplinePoint& p : s.points) points.push_back(encode_spline_point(p));
    return {{"kind", std::string(spline::kind_name(s.kind))},
            {"closed", s.closed},
            {"follow_ground", s.follow_ground},
            {"ground_offset", s.ground_offset},
            {"network", s.network},
            {"id", s.id},
            {"points", std::move(points)}};
}

void decode_spline(JsonFields& f, Spline& s) {
    f.enumeration("kind", s.kind, k_kinds);
    f.boolean("closed", s.closed).boolean("follow_ground", s.follow_ground);
    f.number("ground_offset", s.ground_offset, -1.0e4F, 1.0e4F);
    f.integer("network", s.network, 0, 0xFFFFFFFFLL).integer("id", s.id, 0, 0xFFFFFFFFLL);
    if (const json* pts = f.raw("points"); pts != nullptr) {
        if (!pts->is_array()) {
            f.fail("points", "expected an array of points");
        } else if (pts->size() > spline::k_max_points) {
            f.fail("points", "at most " + std::to_string(spline::k_max_points) + " points");
        } else {
            std::vector<SplinePoint> out;
            out.reserve(pts->size());
            for (std::size_t i = 0; i < pts->size(); ++i) {
                SplinePoint p;
                std::string error;
                if (!decode_spline_point((*pts)[i], "points[" + std::to_string(i) + "]", f.strict(), p, error)) {
                    fail_with_path(f, error);
                    return;
                }
                out.push_back(p);
            }
            s.points = std::move(out);
        }
    }
    if (!f.ok()) return;
    // A spline without points is a valid component ("no curve yet": added
    // empty, then drawn or given points); its cache reports why it does not
    // compile. Once it has points, the whole spline must make a curve.
    if (s.points.empty()) return;
    if (auto valid = spline::validate(s); !valid) fail_with_path(f, valid.error().message);
}

} // namespace engine::scene::detail
