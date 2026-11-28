#include "engine/scene/dynamic_mesh_api.hpp"

#include <format>

#include "engine/core/json_fields.hpp"
#include "engine/geometry/mesh_ops.hpp"
#include "engine/geometry/polygon.hpp"
#include "engine/scene/geometry_spec.hpp"

namespace engine::scene {

namespace {

using core::JsonFields;
using Json = nlohmann::json;

constexpr std::size_t k_max_polygons = 256;
constexpr std::size_t k_max_ring_points = 4096;
constexpr std::size_t k_max_raw_vertices = std::size_t{1} << 18;

std::unexpected<core::Error> fail(std::string message) { return std::unexpected(core::make_error(std::move(message))); }

Json vec3_json(glm::vec3 v) { return Json::array({v.x, v.y, v.z}); }

Json bounds_json(const asset::Aabb& b) { return {{"min", vec3_json(b.min)}, {"max", vec3_json(b.max)}}; }

std::string hash_hex(std::uint64_t h) { return std::format("{:016x}", h); }

Json ring_json(const geometry::Ring& ring) {
    Json out = Json::array();
    for (const glm::dvec2 p : ring) {
        out.push_back(Json::array({p.x, p.y}));
    }
    return out;
}

std::expected<geometry::PolygonSet, core::Error> read_polygons(const Json* v, std::string_view key) {
    geometry::PolygonSet set;
    if (v == nullptr || v->is_null()) {
        return set;
    }
    if (!v->is_array() || v->size() > k_max_polygons) {
        return fail(std::format("{}: expected an array of at most {} polygons", key, k_max_polygons));
    }
    for (std::size_t i = 0; i < v->size(); ++i) {
        JsonFields f((*v)[i], std::format("{}[{}]", key, i));
        geometry::Polygon p;
        f.dvec2_list("outer", p.outer, k_max_ring_points);
        if (const Json* holes = f.raw("holes"); holes != nullptr && !holes->is_null()) {
            bool ok = holes->is_array() && holes->size() <= k_max_polygons;
            for (std::size_t h = 0; ok && h < holes->size(); ++h) {
                geometry::Ring ring;
                ok = JsonFields::read_dvec2_list((*holes)[h], ring, k_max_ring_points);
                p.holes.push_back(std::move(ring));
            }
            if (!ok) {
                f.fail("holes", "expected an array of rings of [x, y] points");
            }
        }
        if (auto done = f.finish(); !done) {
            return std::unexpected(done.error());
        }
        set.push_back(std::move(p));
    }
    return set;
}

Json polygons_json(const geometry::PolygonSet& set) {
    Json polygons = Json::array();
    double total = 0.0;
    for (const geometry::Polygon& p : set) {
        Json holes = Json::array();
        for (const geometry::Ring& h : p.holes) {
            holes.push_back(ring_json(h));
        }
        polygons.push_back({{"outer", ring_json(p.outer)}, {"holes", std::move(holes)}});
        total += geometry::area(p);
    }
    return {{"polygons", std::move(polygons)}, {"area", total}};
}

Json measure_json(const geometry::MeshData& m) {
    return {{"vertices", m.vertices.size()},
            {"triangles", m.triangle_count()},
            {"bounds", bounds_json(geometry::compute_bounds(m))},
            {"hash", hash_hex(geometry::mesh_hash(m))}};
}

std::expected<std::string, core::Error> read_id(JsonFields& f) {
    std::string id;
    const bool has = f.has("id");
    f.string("id", id, 128);
    if (!f.ok()) {
        return std::unexpected(f.finish().error());
    }
    if (!has || !valid_dynamic_mesh_id(id)) {
        return fail("id: required, 1-128 printable ASCII characters without spaces");
    }
    return id;
}

std::expected<Json, core::Error> info_of(const DynamicMeshStore& store, const std::string& id) {
    const std::optional<DynamicMeshInfo> info = store.info(id);
    if (!info) {
        return fail(std::format("id: no dynamic mesh \"{}\" (mesh.dynamic.list shows them)", id));
    }
    return dynamic_mesh_info_json(*info);
}

// [x, y, z] float lists (positions / normals) of a raw mesh.
bool read_points(JsonFields& f, std::string_view key, std::vector<glm::vec3>& out) {
    f.vec3_list(key, out, k_max_raw_vertices);
    return f.ok();
}

} // namespace

Json dynamic_mesh_info_json(const DynamicMeshInfo& i) {
    return {{"id", i.id},
            {"version", i.version},
            {"resident_version", i.resident_version},
            {"resident", i.resident_version == i.version},
            {"vertices", i.vertices},
            {"triangles", i.triangles},
            {"bytes", i.bytes},
            {"bounds", bounds_json(i.bounds)},
            {"hash", hash_hex(i.hash)}};
}

std::expected<Json, core::Error> geometry_generate(DynamicMeshStore* store, const Json& params) {
    JsonFields f(params, "");
    const Json* spec = f.raw("spec");
    std::string id;
    const bool has_id = f.has("id");
    f.string("id", id, 128);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    if (spec == nullptr) {
        return fail("spec: required (a generator spec: {\"type\": \"roof\" | \"sweep\" | \"extrude\" | \"stone\", ...})");
    }
    auto mesh = generate_from_spec(*spec, "spec");
    if (!mesh) {
        return std::unexpected(mesh.error());
    }
    if (!has_id) {
        return measure_json(*mesh);
    }
    if (store == nullptr) {
        return fail("id: this build has no dynamic mesh store (no renderer)");
    }
    auto version = store->submit(id, std::move(*mesh));
    if (!version) {
        return std::unexpected(version.error());
    }
    return info_of(*store, id);
}

std::expected<Json, core::Error> geometry_polygon(const Json& params) {
    JsonFields f(params, "");
    std::string op;
    double delta = 0.0;
    double epsilon = 0.01;
    geometry::OffsetOptions options;
    geometry::FillRule fill = geometry::FillRule::non_zero;
    constexpr core::EnumName<geometry::JoinType> k_joins[] = {
        {"miter", geometry::JoinType::miter}, {"round", geometry::JoinType::round}, {"square", geometry::JoinType::square}};
    constexpr core::EnumName<geometry::FillRule> k_fills[] = {{"non_zero", geometry::FillRule::non_zero},
                                                             {"even_odd", geometry::FillRule::even_odd}};
    f.string("op", op, 16)
        .number("delta", delta, -geometry::kPolygonMaxCoordinate, geometry::kPolygonMaxCoordinate)
        .number("epsilon", epsilon, 0.0, 1.0e3)
        .enumeration("join", options.join, k_joins)
        .number("miter_limit", options.miter_limit, 1.0, 100.0)
        .number("arc_tolerance", options.arc_tolerance, 1.0e-4, 10.0)
        .enumeration("fill", fill, k_fills);
    auto subject = read_polygons(f.raw("subject"), "subject");
    auto clip = read_polygons(f.raw("clip"), "clip");
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    if (!subject) {
        return std::unexpected(subject.error());
    }
    if (!clip) {
        return std::unexpected(clip.error());
    }
    std::expected<geometry::PolygonSet, core::Error> result = fail("");
    if (op == "offset") {
        result = geometry::offset(*subject, delta, options);
    } else if (op == "union" || op == "intersect" || op == "subtract" || op == "xor") {
        const geometry::BooleanOp b = op == "union"       ? geometry::BooleanOp::unite
                                      : op == "intersect" ? geometry::BooleanOp::intersect
                                      : op == "subtract"  ? geometry::BooleanOp::subtract
                                                          : geometry::BooleanOp::exclusive_or;
        result = geometry::boolean(b, *subject, *clip);
    } else if (op == "repair") {
        std::vector<geometry::Ring> rings;
        for (const geometry::Polygon& p : *subject) {
            rings.push_back(p.outer);
            rings.insert(rings.end(), p.holes.begin(), p.holes.end());
        }
        result = geometry::repair(rings, fill);
    } else if (op == "simplify") {
        result = geometry::simplify(*subject, epsilon);
    } else {
        return fail(std::format("op: expected offset, union, intersect, subtract, xor, repair or simplify (got \"{}\")", op));
    }
    if (!result) {
        return std::unexpected(result.error());
    }
    return polygons_json(*result);
}

std::expected<Json, core::Error> dynamic_mesh_set(DynamicMeshStore& store, const Json& params) {
    JsonFields f(params, "");
    auto id = read_id(f);
    if (!id) {
        return std::unexpected(id.error());
    }
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<double> uvs;
    std::vector<double> indices;
    double crease = 40.0;
    read_points(f, "positions", positions);
    read_points(f, "normals", normals);
    f.number_list("uvs", uvs, 2 * k_max_raw_vertices)
        .number_list("indices", indices, 6 * k_max_raw_vertices)
        .number("crease_angle", crease, 0.0, 180.0);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    if (!normals.empty() && normals.size() != positions.size()) {
        return fail(std::format("normals: {} given for {} positions", normals.size(), positions.size()));
    }
    if (!uvs.empty() && uvs.size() != 2 * positions.size()) {
        return fail(std::format("uvs: expected {} numbers (u, v per position), got {}", 2 * positions.size(), uvs.size()));
    }
    geometry::MeshData mesh;
    mesh.vertices.resize(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        mesh.vertices[i].position = positions[i];
        if (!normals.empty()) {
            mesh.vertices[i].normal = glm::length(normals[i]) > 0.0F ? glm::normalize(normals[i]) : normals[i];
        }
        if (!uvs.empty()) {
            mesh.vertices[i].uv = glm::vec2(static_cast<float>(uvs[2 * i]), static_cast<float>(uvs[2 * i + 1]));
        }
    }
    for (const double v : indices) {
        if (v < 0.0 || v >= static_cast<double>(positions.size()) || v != static_cast<double>(static_cast<std::uint32_t>(v))) {
            return fail(std::format("indices: {} is not a vertex index below {}", v, positions.size()));
        }
        mesh.indices.push_back(static_cast<std::uint32_t>(v));
    }
    if (mesh.indices.size() % 3 != 0) {
        return fail(std::format("indices: {} is not a whole number of triangles", mesh.indices.size()));
    }
    if (normals.empty()) {
        geometry::compute_normals(mesh, geometry::NormalOptions{static_cast<float>(crease)});
    }
    if (uvs.empty()) {
        geometry::generate_uvs(mesh);
    }
    if (auto tangents = geometry::compute_tangents(mesh); !tangents) {
        return std::unexpected(tangents.error());
    }
    if (auto version = store.submit(*id, std::move(mesh)); !version) {
        return std::unexpected(version.error());
    }
    return info_of(store, *id);
}

std::expected<Json, core::Error> dynamic_mesh_update(DynamicMeshStore& store, const Json& params) {
    JsonFields f(params, "");
    auto id = read_id(f);
    if (!id) {
        return std::unexpected(id.error());
    }
    std::uint32_t first = 0;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    f.integer("first", first, 0, 0xFFFFFFFFLL);
    read_points(f, "positions", positions);
    read_points(f, "normals", normals);
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    const geometry::MeshData* current = store.mesh(*id);
    if (current == nullptr) {
        return fail(std::format("id: no dynamic mesh \"{}\"", *id));
    }
    if (!normals.empty() && normals.size() != positions.size()) {
        return fail(std::format("normals: {} given for {} positions", normals.size(), positions.size()));
    }
    if (static_cast<std::size_t>(first) + positions.size() > current->vertices.size()) {
        return fail(std::format("positions: vertices [{}, {}) are outside the mesh's {}", first,
                                static_cast<std::size_t>(first) + positions.size(), current->vertices.size()));
    }
    std::vector<asset::Vertex> vertices(current->vertices.begin() + first,
                                        current->vertices.begin() + first + static_cast<std::ptrdiff_t>(positions.size()));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        vertices[i].position = positions[i];
        if (!normals.empty()) {
            vertices[i].normal = glm::length(normals[i]) > 0.0F ? glm::normalize(normals[i]) : normals[i];
        }
    }
    if (auto version = store.update_vertices(*id, first, vertices); !version) {
        return std::unexpected(version.error());
    }
    return info_of(store, *id);
}

std::expected<Json, core::Error> dynamic_mesh_list(const DynamicMeshStore& store, const Json& params) {
    JsonFields f(params, "");
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    Json meshes = Json::array();
    for (const DynamicMeshInfo& i : store.list()) {
        meshes.push_back(dynamic_mesh_info_json(i));
    }
    const DynamicMeshStats s = store.stats();
    return Json{{"meshes", std::move(meshes)},
                {"stats",
                 {{"meshes", s.meshes},
                  {"pending_uploads", s.pending_uploads},
                  {"pending_bytes", s.pending_bytes},
                  {"uploaded_bytes_last_frame", s.uploaded_bytes_last_frame},
                  {"resident_bytes", s.resident_bytes},
                  {"frame_byte_budget", store.config().frame_byte_budget}}}};
}

std::expected<Json, core::Error> dynamic_mesh_get(const DynamicMeshStore& store, const Json& params) {
    JsonFields f(params, "");
    auto id = read_id(f);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    return info_of(store, *id);
}

std::expected<Json, core::Error> dynamic_mesh_remove(DynamicMeshStore& store, const Json& params) {
    JsonFields f(params, "");
    auto id = read_id(f);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (auto done = f.finish(); !done) {
        return std::unexpected(done.error());
    }
    return Json{{"removed", store.remove(*id)}};
}

} // namespace engine::scene
