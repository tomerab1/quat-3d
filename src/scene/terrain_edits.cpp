// Terrain edit layers in the scene (G19.1): the component codec and the
// TerrainEdits service.

#include "engine/scene/terrain_edits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

using json = nlohmann::json;
using core::JsonFields;
using terrain::StampOp;
using terrain::TerrainStamp;

[[nodiscard]] json vec2_json(glm::vec2 v) { return json::array({v.x, v.y}); }

[[nodiscard]] bool read_vec2(const json& v, glm::vec2& out) {
    return v.is_array() && v.size() == 2 && JsonFields::read_float(v[0], out.x) &&
           JsonFields::read_float(v[1], out.y);
}

[[nodiscard]] core::Error error(std::string what) { return core::Error{std::move(what)}; }

// World matrix from the local chain (current even before transform_system).
[[nodiscard]] glm::vec3 world_position(const entt::registry& r, entt::entity e) {
    glm::mat4 m(1.0F);
    std::size_t guard = 0;
    for (entt::entity a = e; a != entt::null && r.valid(a) && guard < 100'000; ++guard) {
        if (const auto* t = r.try_get<Transform>(a)) m = t->local * m;
        const auto* p = r.try_get<Parent>(a);
        a = p != nullptr ? p->entity : entt::entity{entt::null};
    }
    return glm::vec3(m[3]);
}

} // namespace

// ---- JSON --------------------------------------------------------------------

json encode_stamp(const TerrainStamp& s) {
    json points = json::array();
    for (const glm::vec2& p : s.points) points.push_back(vec2_json(p));
    json out = {{"id", s.id},
                {"shape", terrain::to_string(s.shape)},
                {"points", std::move(points)},
                {"radius", s.radius},
                {"falloff", s.falloff},
                {"op", terrain::to_string(s.op)},
                {"strength", s.strength}};
    switch (s.op) {
    case StampOp::flatten:
        out["height"] = s.height;
        out["pivot"] = vec2_json(s.pivot);
        out["normal"] = json::array({s.normal.x, s.normal.y, s.normal.z});
        break;
    case StampOp::raise:
    case StampOp::lower:
        out["amount"] = s.amount;
        break;
    case StampOp::smooth:
        out["iterations"] = s.iterations;
        break;
    case StampOp::paint:
        out["channel"] = terrain::to_string(s.channel);
        out["value"] = s.value;
        break;
    }
    return out;
}

void decode_stamp(JsonFields& f, TerrainStamp& s) {
    const auto name = [&f](std::string_view key, auto& out, auto parse, const char* names) {
        std::string text;
        if (!f.has(key)) return;
        f.string(key, text, 32);
        if (!f.ok()) return;
        if (const auto v = parse(text)) {
            out = *v;
        } else {
            f.fail(key, std::string("expected one of ") + names);
        }
    };
    f.integer("id", s.id, 0, std::numeric_limits<std::int64_t>::max());
    name("shape", s.shape, terrain::stamp_shape_from_string, "circle, capsule, polygon");
    name("op", s.op, terrain::stamp_op_from_string, "flatten, raise, lower, smooth, paint");
    name("channel", s.channel, terrain::paint_channel_from_string, "dirt, gravel, soil, grass");
    if (const json* pts = f.raw("points")) {
        if (!pts->is_array() || pts->size() > terrain::max_stamp_points) {
            f.fail("points", "expected an array of at most " +
                                 std::to_string(terrain::max_stamp_points) + " [x, z] pairs");
        } else {
            std::vector<glm::vec2> points(pts->size());
            for (std::size_t i = 0; i < pts->size(); ++i) {
                if (!read_vec2((*pts)[i], points[i])) {
                    f.fail("points", "[" + std::to_string(i) + "]: expected [x, z] (metres)");
                    break;
                }
            }
            s.points = std::move(points);
        }
    }
    f.number("radius", s.radius, 0.0F, terrain::max_stamp_extent_m)
        .number("falloff", s.falloff, 0.0F, terrain::max_stamp_extent_m)
        .number("strength", s.strength, 0.0F, 1.0F)
        .number("height", s.height, -terrain::max_stamp_coordinate_m,
                terrain::max_stamp_coordinate_m)
        .number("amount", s.amount, 0.0F, terrain::max_stamp_amount_m)
        .number("value", s.value, 0.0F, 1.0F)
        .integer("iterations", s.iterations, 1, terrain::max_smooth_iterations)
        .vec3("normal", s.normal);
    if (const json* pivot = f.raw("pivot")) {
        if (!read_vec2(*pivot, s.pivot)) f.fail("pivot", "expected [x, z] (metres)");
    }
    if (!f.ok()) return;
    if (auto ok = terrain::validate_stamp(s); !ok) {
        // validate_stamp's message starts with the field name.
        const std::string& m = ok.error();
        const std::size_t colon = m.find(':');
        f.fail(colon == std::string::npos ? "stamp" : m.substr(0, colon),
               colon == std::string::npos ? m : m.substr(colon + 2));
    }
}

json encode_terrain_edits(const TerrainEditLayer& layer) {
    json stamps = json::array();
    for (const TerrainStamp& s : layer.list.stamps()) stamps.push_back(encode_stamp(s));
    return {{"stamps", std::move(stamps)}, {"next_id", layer.list.next_id()}};
}

void decode_terrain_edits(JsonFields& f, TerrainEditLayer& layer) {
    std::uint64_t next_id = 0;
    f.integer("next_id", next_id, 0, std::numeric_limits<std::int64_t>::max());
    const json* arr = f.raw("stamps");
    if (arr == nullptr) {
        if (f.ok() && next_id != 0) {
            std::vector<TerrainStamp> keep = layer.list.stamps();
            if (auto ok = layer.list.assign(std::move(keep), next_id); !ok) {
                f.fail("next_id", ok.error());
            }
        }
        return;
    }
    if (!arr->is_array() || arr->size() > terrain::max_stamps) {
        f.fail("stamps", "expected an array of at most " + std::to_string(terrain::max_stamps) +
                             " stamps");
        return;
    }
    std::vector<TerrainStamp> stamps;
    stamps.reserve(arr->size());
    std::uint64_t auto_id = 1;
    for (const json& v : *arr) {
        if (v.is_object() && v.contains("id")) {
            std::int64_t id = 0;
            if (JsonFields::read_int(v["id"], id) && id > 0) {
                auto_id = std::max(auto_id, static_cast<std::uint64_t>(id) + 1);
            }
        }
    }
    for (std::size_t i = 0; i < arr->size(); ++i) {
        const std::string ctx = "terrain_edits.stamps[" + std::to_string(i) + "]";
        JsonFields sf((*arr)[i], ctx, f.strict());
        TerrainStamp s;
        decode_stamp(sf, s);
        if (auto ok = sf.finish(); !ok) {
            f.fail("stamps", ok.error().message);
            return;
        }
        if (s.id == 0) s.id = auto_id++; // hand-written files may omit ids
        stamps.push_back(std::move(s));
    }
    if (auto ok = layer.list.assign(std::move(stamps), next_id); !ok) f.fail("stamps", ok.error());
}

// ---- Service -------------------------------------------------------------------

std::optional<TerrainAnchor> find_terrain_anchor(const entt::registry& registry) {
    const auto view = registry.view<const Terrain>();
    if (view.begin() == view.end()) return std::nullopt;
    const entt::entity e = *view.begin();
    return TerrainAnchor{e, world_position(registry, e)};
}

TerrainStamp stamp_to_local(TerrainStamp s, const glm::vec3& anchor) {
    const glm::vec2 a(anchor.x, anchor.z);
    for (glm::vec2& p : s.points) p -= a;
    s.pivot -= a;
    s.height -= anchor.y;
    return s;
}

TerrainStamp stamp_to_world(TerrainStamp s, const glm::vec3& anchor) {
    const glm::vec2 a(anchor.x, anchor.z);
    for (glm::vec2& p : s.points) p += a;
    s.pivot += a;
    s.height += anchor.y;
    return s;
}

std::expected<std::pair<TerrainAnchor, TerrainEditLayer*>, core::Error>
TerrainEdits::layer(bool create) {
    const std::optional<TerrainAnchor> anchor = find_terrain_anchor(*registry_);
    if (!anchor) {
        return std::unexpected(error(
            "the scene has no terrain (no entity has a `terrain` component); terrain edits "
            "apply to the terrain"));
    }
    auto* layer = registry_->try_get<TerrainEditLayer>(anchor->entity);
    if (layer == nullptr && create) layer = &registry_->emplace<TerrainEditLayer>(anchor->entity);
    return std::pair{*anchor, layer};
}

std::expected<std::uint64_t, core::Error>
TerrainEdits::add(const TerrainStamp& world_stamp, std::optional<std::uint64_t> before) {
    auto l = layer(true);
    if (!l) return std::unexpected(l.error());
    auto id = l->second->list.add(stamp_to_local(world_stamp, l->first.position), before);
    if (!id) return std::unexpected(error(id.error()));
    return *id;
}

std::expected<void, core::Error> TerrainEdits::update(std::uint64_t id,
                                                      const TerrainStamp& world_stamp) {
    auto l = layer(false);
    if (!l) return std::unexpected(l.error());
    if (l->second == nullptr) {
        return std::unexpected(error("id: no stamp with id " + std::to_string(id)));
    }
    auto ok = l->second->list.update(id, stamp_to_local(world_stamp, l->first.position));
    if (!ok) return std::unexpected(error(ok.error()));
    return {};
}

std::expected<void, core::Error> TerrainEdits::remove(std::uint64_t id) {
    auto l = layer(false);
    if (!l) return std::unexpected(l.error());
    if (l->second == nullptr || !l->second->list.remove(id)) {
        return std::unexpected(error("id: no stamp with id " + std::to_string(id)));
    }
    return {};
}

std::size_t TerrainEdits::clear() {
    auto l = layer(false);
    if (!l || l->second == nullptr) return 0;
    const std::size_t n = l->second->list.stamps().size();
    l->second->list.clear();
    return n;
}

std::vector<TerrainStamp> TerrainEdits::list() const {
    std::vector<TerrainStamp> out;
    const std::optional<TerrainAnchor> anchor = find_terrain_anchor(*registry_);
    if (!anchor) return out;
    if (const auto* l = registry_->try_get<TerrainEditLayer>(anchor->entity)) {
        out.reserve(l->list.stamps().size());
        for (const TerrainStamp& s : l->list.stamps()) {
            out.push_back(stamp_to_world(s, anchor->position));
        }
    }
    return out;
}

std::optional<TerrainStamp> TerrainEdits::get(std::uint64_t id) const {
    const std::optional<TerrainAnchor> anchor = find_terrain_anchor(*registry_);
    if (!anchor) return std::nullopt;
    const auto* l = registry_->try_get<TerrainEditLayer>(anchor->entity);
    const TerrainStamp* s = l != nullptr ? l->list.find(id) : nullptr;
    if (s == nullptr) return std::nullopt;
    return stamp_to_world(*s, anchor->position);
}

std::uint64_t TerrainEdits::revision() const {
    const std::optional<TerrainAnchor> anchor = find_terrain_anchor(*registry_);
    if (!anchor) return 0;
    const auto* l = registry_->try_get<TerrainEditLayer>(anchor->entity);
    return l != nullptr ? l->list.revision() : 0;
}

} // namespace engine::scene
