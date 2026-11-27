#pragma once

// Procedural scattering (ROADMAP R9.2): turns a Scatter component into the
// placements of the entity's InstancedMesh.
//
// Generation runs on the CPU, deliberately: it happens only when a scatter's
// parameters, transform or ground change (never per frame), Poisson-disc
// sampling (Bridson) is inherently sequential, and its consumers — per-
// instance culling, scene files, the remote API, the inspector — all read
// the placements on the CPU. A GPU path becomes worthwhile with GPU-driven
// culling (R9.4), where placements could live only in GPU memory; this
// deterministic implementation then stays the reference.
//
// Determinism: candidates depend only on (seed, area, density, spacing,
// pattern, max_instances) and are produced by a seeded PCG; each candidate's
// scale/yaw come from a hash of (seed, candidate index). The ground only
// decides which candidates survive, so streaming terrain in never moves the
// placements that already exist.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/scene/components.hpp"

namespace engine::terrain {
struct Heightmap;
}

namespace engine::scene {

// Hard cap on one scatter's placements (Scatter::max_instances is clamped to it).
inline constexpr std::uint32_t k_scatter_instance_limit = 100'000;

struct GroundSample {
    float     height = 0.0F;       // world Y, metres
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
};

// Where scatters put things. `sample` returns the ground at world (x, z), or
// nullopt where no ground is known (an unloaded streamed tile: the candidate
// is skipped). An empty `sample` means "no terrain": flat ground at the
// scatter entity's own height. `revision` returns a value that changes
// whenever the ground inside the world XZ rectangle [min, max] changes (a
// tile loads, unloads or regenerates); empty means the ground never changes.
struct ScatterGround {
    std::function<std::optional<GroundSample>(float x, float z)>       sample;
    std::function<std::uint64_t(glm::vec2 min_xz, glm::vec2 max_xz)> revision;
};

// A terrain heightmap tile placed in the world: `origin` is its minimum
// corner (heights are offset by origin.y), as TerrainStreamer::tile_origin
// gives it. The map must outlive the ground built from it.
struct GroundTile {
    const terrain::Heightmap* map = nullptr;
    glm::vec3                 origin{0.0F};
};

// Ground over a set of tiles (a single editing tile, or the streamed window).
// Revisions fingerprint each overlapping tile's placement, size, height range
// and a sparse set of its heights.
[[nodiscard]] ScatterGround make_tile_ground(std::vector<GroundTile> tiles);

// The world XZ rectangle a scatter covers when its entity sits at `centre`.
struct ScatterRect {
    glm::vec2 min{0.0F};
    glm::vec2 max{0.0F};
};
[[nodiscard]] ScatterRect scatter_rect(const Scatter& scatter, const glm::vec3& centre);

// Candidate positions as XZ offsets (metres) from the area's centre, in
// generation order. Depends only on seed / area / size / radius / density /
// min_spacing / pattern / max_instances. Every pair is at least
// `min_spacing` apart; the count is round(density * area), capped by
// max_instances, or fewer when min_spacing cannot fit that many.
[[nodiscard]] std::vector<glm::vec2> scatter_candidates(const Scatter& scatter);

// Placements for an entity whose world matrix is `entity_world`, relative to
// the entity (entity_world * instance = world placement). The entity's
// translation is the area's centre; its rotation and scale are folded out of
// the instances (scatter entities should be uniformly scaled: a non-uniform
// scale would shear tilted placements).
[[nodiscard]] std::vector<InstanceTransform>
generate_scatter(const Scatter& scatter, const glm::mat4& entity_world, const ScatterGround& ground);

// Key of everything a generation depends on (never 0).
[[nodiscard]] std::uint64_t scatter_key(const Scatter& scatter, const glm::mat4& entity_world,
                                        std::uint64_t ground_revision);

// Regenerates every Scatter (with a Transform) whose key changed, writing
// InstancedMesh::instances. Reads Transform::world, so run it after
// transform_system. Returns the number of scatters regenerated.
std::size_t scatter_system(entt::registry& registry, const ScatterGround& ground);

} // namespace engine::scene
