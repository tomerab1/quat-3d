// Skeleton runtime: bind-pose composition + hierarchy walk (Phase 5, Slice 5.1).

#include "engine/animation/skeleton.hpp"


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

    // Resolve each joint after its ancestors, whatever order the glTF listed
    // them in: walk up to the first resolved ancestor, then compose down.
    std::vector<std::size_t> chain;
    for (std::size_t i = 0; i < n; ++i) {
        chain.clear();
        for (std::size_t j = i; resolved[j] == 0 && chain.size() <= n;) {
            chain.push_back(j);
            const int parent = skeleton.joints[j].parent;
            if (parent < 0) break;
            j = static_cast<std::size_t>(parent);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (resolved[*it] != 0) continue;
            const int parent = skeleton.joints[*it].parent;
            world[*it] = (parent < 0 ? root_transform : world[static_cast<std::size_t>(parent)]) *
                         local[*it];
            resolved[*it] = 1;
        }
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
