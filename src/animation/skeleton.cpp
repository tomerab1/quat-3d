// Skeleton runtime: bind-pose composition + hierarchy walk (Phase 5, Slice 5.1).

#include "engine/animation/skeleton.hpp"

#include <functional>

#include <glm/gtc/matrix_transform.hpp>

namespace engine::animation {

glm::mat4 SkeletonAsset::local_bind(std::size_t i) const {
    const Joint& j = joints[i];
    return glm::translate(glm::mat4(1.0F), j.translation) * glm::mat4_cast(j.rotation) *
           glm::scale(glm::mat4(1.0F), j.scale);
}

std::vector<glm::mat4> compute_world_matrices(const SkeletonAsset& skeleton,
                                              std::span<const glm::mat4> local,
                                              const glm::mat4& root_transform) {
    const std::size_t n = skeleton.joints.size();
    std::vector<glm::mat4> world(n, glm::mat4(1.0F));
    std::vector<char> resolved(n, 0);

    // Recursive resolution so the result is correct regardless of whether the
    // glTF listed parents before children.
    std::function<void(std::size_t)> resolve = [&](std::size_t i) {
        if (resolved[i] != 0) return;
        const int parent = skeleton.joints[i].parent;
        if (parent < 0) {
            world[i] = root_transform * local[i];
        } else {
            resolve(static_cast<std::size_t>(parent));
            world[i] = world[static_cast<std::size_t>(parent)] * local[i];
        }
        resolved[i] = 1;
    };
    for (std::size_t i = 0; i < n; ++i) {
        resolve(i);
    }
    return world;
}

std::vector<glm::mat4> compute_bind_world(const SkeletonAsset& skeleton) {
    std::vector<glm::mat4> local(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        local[i] = skeleton.local_bind(i);
    }
    return compute_world_matrices(skeleton, local);
}

} // namespace engine::animation
