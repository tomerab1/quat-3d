#pragma once

// Load-time clip compression for the glTF loader (private to engine_scene;
// fastgltf-free so tests can drive it directly).

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"

namespace engine::scene {

// ACL-compresses each clip against the skin owning most of its channels
// (skin_roots: each skin's armature global, for its scale). The compressed
// tracks are addressed by node, so skins over a subset of those joints play
// them too; clips that drive nodes outside that skin keep their raw keys.
// Clips that cannot be compressed stay raw; one summary line goes to stderr.
void compress_clips(std::vector<animation::AnimClipAsset>& clips,
                    const std::vector<animation::SkeletonAsset>& skins,
                    const std::vector<glm::mat4>& skin_roots, const std::string& label);

} // namespace engine::scene
