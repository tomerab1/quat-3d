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
                                              std::span<const glm::mat4> local) {
    const std::size_t n = skeleton.joints.size();
    std::vector<glm::mat4> world(n, glm::mat4(1.0F));
    std::vector<char> resolved(n, 0);

    // Recursive resolution so the result is correct regardless of whether the
    // glTF listed parents before children.
    std::function<void(std::size_t)> resolve = [&](std::size_t i) {
        if (resolved[i] != 0) return;
        const int parent = skeleton.joints[i].parent;
        if (parent < 0) {
            world[i] = local[i];
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

std::expected<void, core::Error> run_skeleton_self_test() {
    // Root at the origin; child translated +2 on X, parented under the root.
    SkeletonAsset s;
    s.joints.resize(2);
    s.joints[0].name = "root";
    s.joints[0].parent = -1;
    s.joints[1].name = "child";
    s.joints[1].parent = 0;
    s.joints[1].translation = {2.0F, 0.0F, 0.0F};

    // Bind-pose world matrices compose through the hierarchy.
    const std::vector<glm::mat4> world = compute_bind_world(s);
    if (world.size() != 2) {
        return std::unexpected(core::Error{"skeleton self-test: wrong world matrix count"});
    }
    if (glm::distance(glm::vec3(world[1][3]), glm::vec3(2.0F, 0.0F, 0.0F)) > 1e-5F) {
        return std::unexpected(core::Error{"skeleton self-test: child world translation wrong"});
    }

    // With inverse-bind = inverse(world_bind), the skinning matrix in bind pose is
    // identity (a bind-pose vertex is unmoved) — the defining property.
    for (std::size_t i = 0; i < s.joints.size(); ++i) {
        s.joints[i].inverse_bind = glm::inverse(world[i]);
    }
    for (std::size_t i = 0; i < s.joints.size(); ++i) {
        const glm::mat4 skinning = world[i] * s.joints[i].inverse_bind;
        if (glm::distance(skinning[3], glm::vec4(0.0F, 0.0F, 0.0F, 1.0F)) > 1e-5F) {
            return std::unexpected(
                core::Error{"skeleton self-test: bind-pose skinning matrix is not identity"});
        }
    }
    return {};
}

} // namespace engine::animation
