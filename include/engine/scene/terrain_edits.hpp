#pragma once

// Terrain edit layers in the scene (ROADMAP G19.1, docs/terrain-edits.md).
//
// The `terrain_edits` component on the Terrain entity holds the ordered
// stamp list (engine/terrain/edits.hpp) in terrain-local coordinates; scene
// files and saves serialise it through the codec below. The application's
// terrain runtime diffs it every frame and recomputes only the regions that
// changed (terrain pass, Jolt height fields, Scatter revisions, the grass
// mask, nav regions).
//
// `TerrainEdits` is the service game modules (G17.1), Lua and the remote
// `terrain.edit.*` commands use: world-space stamps in, ids out. It has no
// graphics types and touches the registry only (main thread).

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/terrain/edits.hpp"

namespace engine::scene {

// The edit layer of the Terrain entity it sits on.
struct TerrainEditLayer {
    terrain::TerrainEditList list{};
};

// ---- JSON (component codec, remote, saves) ---------------------------------

// One stamp: {id?, shape, points:[[x, z]...], radius, falloff, op, strength,
// height, pivot, normal, amount, iterations, channel, value}. Only the
// fields of the stamp's op are written. Decoding reads into `out` (fields
// left out keep its values); the result is validated.
[[nodiscard]] nlohmann::json encode_stamp(const terrain::TerrainStamp& stamp);
void decode_stamp(core::JsonFields& fields, terrain::TerrainStamp& out);

// The component: {stamps:[...], next_id}.
[[nodiscard]] nlohmann::json encode_terrain_edits(const TerrainEditLayer& layer);
void decode_terrain_edits(core::JsonFields& fields, TerrainEditLayer& layer);

// ---- Service ---------------------------------------------------------------

// The terrain anchor: world position of the first Terrain entity (tile
// (0,0)'s centre), or nullopt without one.
struct TerrainAnchor {
    entt::entity entity{};
    glm::vec3    position{0.0F};
};
[[nodiscard]] std::optional<TerrainAnchor> find_terrain_anchor(const entt::registry& registry);

// World <-> terrain-local stamp coordinates (points, pivot, height).
[[nodiscard]] terrain::TerrainStamp stamp_to_local(terrain::TerrainStamp stamp,
                                                   const glm::vec3& anchor);
[[nodiscard]] terrain::TerrainStamp stamp_to_world(terrain::TerrainStamp stamp,
                                                   const glm::vec3& anchor);

class TerrainEdits {
public:
    explicit TerrainEdits(entt::registry& registry) : registry_(&registry) {}

    // Adds a world-space stamp at the end of the list, or before stamp
    // `before`. Returns its id (the handle undo and remove use). Fails
    // without a Terrain entity or with an invalid stamp (the message names
    // the field).
    [[nodiscard]] std::expected<std::uint64_t, core::Error>
    add(const terrain::TerrainStamp& world_stamp, std::optional<std::uint64_t> before = {});
    [[nodiscard]] std::expected<void, core::Error> update(std::uint64_t id,
                                                          const terrain::TerrainStamp& world_stamp);
    [[nodiscard]] std::expected<void, core::Error> remove(std::uint64_t id);
    // Removes every stamp; returns how many there were.
    std::size_t clear();

    // The stamps in application order, in world space.
    [[nodiscard]] std::vector<terrain::TerrainStamp> list() const;
    [[nodiscard]] std::optional<terrain::TerrainStamp> get(std::uint64_t id) const;
    // The list's revision (0 without a terrain or edits).
    [[nodiscard]] std::uint64_t revision() const;

private:
    [[nodiscard]] std::expected<std::pair<TerrainAnchor, TerrainEditLayer*>, core::Error>
    layer(bool create);

    entt::registry* registry_;
};

} // namespace engine::scene
