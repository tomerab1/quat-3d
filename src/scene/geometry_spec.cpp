#include "engine/scene/geometry_spec.hpp"

#include <format>
#include <string>

#include "engine/core/json_fields.hpp"
#include "engine/geometry/stone.hpp"
#include "engine/geometry/straight_skeleton.hpp"
#include "engine/geometry/sweep.hpp"

namespace engine::scene {

namespace {

using core::JsonFields;
using Json = nlohmann::json;

constexpr std::size_t k_max_polygon_points = 4096;
constexpr std::size_t k_max_holes = 256;
constexpr std::size_t k_max_profile_points = 1024;

std::expected<geometry::MeshData, core::Error> fail(std::string message) {
    return std::unexpected(core::make_error(std::move(message)));
}

// "polygon" and "holes" of a plan polygon.
void read_polygon(JsonFields& f, geometry::Polygon& polygon) {
    f.dvec2_list("polygon", polygon.outer, k_max_polygon_points);
    if (const Json* holes = f.raw("holes"); holes != nullptr && !holes->is_null()) {
        bool ok = holes->is_array() && holes->size() <= k_max_holes;
        for (std::size_t i = 0; ok && i < holes->size(); ++i) {
            geometry::Ring ring;
            ok = JsonFields::read_dvec2_list((*holes)[i], ring, k_max_polygon_points);
            polygon.holes.push_back(std::move(ring));
        }
        if (!ok) {
            f.fail("holes", std::format("expected at most {} rings of [x, y] points", k_max_holes));
        }
    }
}

std::expected<geometry::MeshData, core::Error> roof(JsonFields& f, const std::string& ctx) {
    geometry::Polygon polygon;
    read_polygon(f, polygon);
    std::vector<double> weights;
    f.number_list("weights", weights, k_max_polygon_points * 2);
    geometry::RoofOptions options;
    f.number("base_height", options.base_height, -1.0e5, 1.0e5)
        .number("height_scale", options.height_scale, 1.0e-3, 1.0e3)
        .number("max_height", options.max_height, 0.0, 1.0e5)
        .number("uv_scale", options.uv_scale, 1.0e-3, 1.0e3);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    auto skeleton = geometry::straight_skeleton(polygon, weights);
    if (!skeleton) {
        return fail(ctx + ": " + skeleton.error().message);
    }
    return geometry::roof_mesh(*skeleton, options);
}

std::expected<geometry::MeshData, core::Error> sweep(JsonFields& f, const std::string& ctx) {
    std::vector<glm::dvec3> path;
    std::vector<glm::dvec2> profile;
    f.dvec3_list("path", path, k_max_polygon_points).dvec2_list("profile", profile, k_max_profile_points);
    geometry::SweepOptions options;
    bool caps = true;
    double crease = options.crease_angle_degrees;
    f.boolean("closed_path", options.closed_path)
        .boolean("closed_profile", options.closed_profile)
        .boolean("caps", caps)
        .number("miter_limit", options.miter_limit, 1.0, 100.0)
        .number("uv_scale", options.uv_scale, 1.0e-3, 1.0e3)
        .number("crease_angle", crease, 0.0, 180.0);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    options.cap_start = options.cap_end = caps && options.closed_profile && !options.closed_path;
    options.crease_angle_degrees = static_cast<float>(crease);
    const std::size_t rings = path.size() + (options.closed_path ? 1 : 0);
    const std::size_t columns = profile.size() + (options.closed_profile ? 1 : 0);
    if (rings * columns > k_geometry_spec_max_vertices) {
        return fail(std::format("{}: {} path points x {} profile points is over the {} vertex limit", ctx,
                                path.size(), profile.size(), k_geometry_spec_max_vertices));
    }
    geometry::FrameOptions frame_options;
    frame_options.closed = options.closed_path;
    auto frames = geometry::path_frames(path, frame_options);
    if (!frames) {
        return fail(ctx + ": " + frames.error().message);
    }
    return geometry::sweep(profile, *frames, options);
}

std::expected<geometry::MeshData, core::Error> extrude(JsonFields& f) {
    geometry::Polygon polygon;
    read_polygon(f, polygon);
    geometry::ExtrudeOptions options;
    bool caps = true;
    f.number("height", options.height, 1.0e-3, 1.0e5)
        .number("base_height", options.base_height, -1.0e5, 1.0e5)
        .number("taper", options.taper, -1.0e3, 1.0e3)
        .boolean("caps", caps)
        .number("uv_scale", options.uv_scale, 1.0e-3, 1.0e3);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    options.cap_top = options.cap_bottom = caps;
    return geometry::extrude(polygon, options);
}

std::expected<geometry::MeshData, core::Error> stone(JsonFields& f) {
    geometry::StoneOptions options;
    std::int64_t seed = 1;
    f.integer("seed", seed, std::int64_t{0}, std::int64_t{9007199254740991})
        .vec3("size", options.size)
        .integer("subdivisions", options.subdivisions, 0U, 5U)
        .number("roundness", options.roundness, 0.0F, 1.0F)
        .number("noise", options.noise_amplitude, 0.0F, 1.0F)
        .integer("chips", options.chips, 0U, 16U)
        .number("chip_depth", options.chip_depth, 0.0F, 1.0F)
        .number("uv_scale", options.uv_scale, 1.0e-3F, 1.0e3F);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    options.seed = static_cast<std::uint64_t>(seed);
    return geometry::generate_stone(options);
}

} // namespace

std::expected<geometry::MeshData, core::Error> generate_from_spec(const nlohmann::json& spec,
                                                                  std::string_view context) {
    const std::string ctx(context);
    JsonFields f(spec, ctx);
    std::string type;
    f.string("type", type, 32);
    if (!f.ok()) {
        return std::unexpected(f.finish().error());
    }
    std::expected<geometry::MeshData, core::Error> mesh = fail("");
    if (type == "roof") {
        mesh = roof(f, ctx);
    } else if (type == "sweep") {
        mesh = sweep(f, ctx);
    } else if (type == "extrude") {
        mesh = extrude(f);
    } else if (type == "stone") {
        mesh = stone(f);
    } else {
        return fail(std::format("{}.type: expected one of roof, sweep, extrude, stone (got \"{}\")", ctx, type));
    }
    if (!mesh) {
        const std::string& message = mesh.error().message;
        return fail(message.starts_with(ctx) ? message : ctx + ": " + message);
    }
    if (mesh->vertices.size() > k_geometry_spec_max_vertices) {
        return fail(std::format("{}: the mesh has {} vertices, over the {} limit", ctx, mesh->vertices.size(),
                                k_geometry_spec_max_vertices));
    }
    return mesh;
}

} // namespace engine::scene
