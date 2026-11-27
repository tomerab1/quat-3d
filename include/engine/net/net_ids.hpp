#pragma once

// Replication ids (S1 audit B3).
//
// Both ends of a session build the same scene (same file / construction), and
// replication matches entities by NetReplicated::net_id. The id must therefore
// be (a) identical for the same entity on both ends and (b) unique within the
// scene. Names alone are not unique — the physics demo spawns a dozen "box"
// entities — so the id hashes a stable key:
//
//     <hierarchy path of names>#<occurrence index>
//
// e.g. "level/crates/box#3" for the fourth entity whose path is
// "level/crates/box". Occurrences are counted in entity-index order, which is
// creation order and therefore the same on both ends of a shared scene. Hash
// collisions between distinct keys are re-probed deterministically, and
// pre-existing ids (authored in a scene file) are kept unless duplicated.

#include <cstdint>
#include <string>
#include <string_view>

#include <entt/entity/fwd.hpp>

namespace engine::net {

struct NetIdReport {
    std::uint32_t tagged = 0;          // entities given a fresh id
    std::uint32_t kept = 0;            // entities that kept a pre-existing, unique id
    std::uint32_t duplicate_names = 0; // entities whose path repeats an earlier one
    std::uint32_t hash_collisions = 0; // re-probes needed for distinct keys
    std::uint32_t duplicate_ids = 0;   // pre-existing ids that were already taken (reassigned)
};

// FNV-1a (32-bit) of `key`, never 0 (0 means "not replicated").
[[nodiscard]] std::uint32_t net_id_hash(std::string_view key);

// "grandparent/parent/name" for a named entity (unnamed ancestors read "?").
[[nodiscard]] std::string net_path(const entt::registry& registry, entt::entity e);

// Tags every named entity with a Transform (except predicted characters, which
// their client owns) with a unique NetReplicated id; entities that already
// carry a non-zero id keep it unless another entity owns it first. Logs a
// summary (and every duplicate) to stderr. Deterministic for a given scene.
NetIdReport assign_net_ids(entt::registry& registry);

} // namespace engine::net
