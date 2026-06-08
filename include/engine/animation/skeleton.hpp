#pragma once

// SkeletonAsset — joint hierarchy + bind pose (Phase 5, Slice 5.1).
//
// A glTF skin flattened into a joint array: each joint stores its name, the
// index of its parent joint (-1 for a root), its local bind-pose TRS, and the
// inverse bind matrix that takes a vertex from model space into the joint's
// local space. The animator (5.3) overrides the local TRS per frame and walks
// this hierarchy to produce skinning matrices (joint_world * inverse_bind).

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/error.hpp"

namespace engine::animation {

// One joint in the skeleton. `parent` indexes into SkeletonAsset::joints, or is
// -1 for a root. The TRS is the bind-pose local transform (the default used when
// no animation channel drives this joint).
struct Joint {
    std::string name;
    int         parent = -1; // index into joints, -1 = root
    int         node = -1;    // source glTF node index (maps clip channels here)

    glm::vec3 translation{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F}; // identity (w, x, y, z)
    glm::vec3 scale{1.0F};

    glm::mat4 inverse_bind{1.0F};
};

struct SkeletonAsset {
    std::vector<Joint> joints;

    [[nodiscard]] bool valid() const noexcept { return !joints.empty(); }
    [[nodiscard]] std::size_t joint_count() const noexcept { return joints.size(); }

    // Local bind-pose matrix for joint `i` (T * R * S).
    [[nodiscard]] glm::mat4 local_bind(std::size_t i) const;
};

// Compose world-space joint matrices from per-joint local matrices, resolving
// parents before children (robust to any joint ordering). `local.size()` must
// equal the joint count. Root joints (parent < 0) are seeded with
// `root_transform` (the skeleton's ancestor/armature world transform).
[[nodiscard]] std::vector<glm::mat4>
compute_world_matrices(const SkeletonAsset& skeleton, std::span<const glm::mat4> local,
                       const glm::mat4& root_transform = glm::mat4(1.0F));

// World-space joint matrices for the bind pose (every joint at its bind TRS).
[[nodiscard]] std::vector<glm::mat4> compute_bind_world(const SkeletonAsset& skeleton);

// Builds a small two-joint skeleton in code and verifies local_bind, the
// hierarchy walk, and that inverse-bind round-trips the bind pose to identity.
[[nodiscard]] std::expected<void, core::Error> run_skeleton_self_test();

} // namespace engine::animation
