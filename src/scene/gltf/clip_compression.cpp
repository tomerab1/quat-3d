// Load-time ACL compression of a glTF's clips against the skins they drive.

#include "clip_compression.hpp"

#include <cmath>
#include <cstdio>

#include "engine/animation/compression.hpp"

namespace engine::scene {

namespace {

// The skin owning most of the clip's channel targets; -1 when none.
[[nodiscard]] int skin_for(const animation::AnimClipAsset& clip,
                           const std::vector<animation::SkeletonAsset>& skins) {
    int best = -1;
    std::size_t best_hits = 0;
    for (std::size_t s = 0; s < skins.size(); ++s) {
        std::size_t hits = 0;
        for (const animation::AnimChannel& ch : clip.channels) {
            for (const animation::Joint& j : skins[s].joints) {
                if (j.node == ch.target_node) {
                    ++hits;
                    break;
                }
            }
        }
        if (hits > best_hits) {
            best_hits = hits;
            best = static_cast<int>(s);
        }
    }
    return best;
}

} // namespace

void compress_clips(std::vector<animation::AnimClipAsset>& clips,
                    const std::vector<animation::SkeletonAsset>& skins,
                    const std::vector<glm::mat4>& skin_roots, const std::string& label) {
    std::size_t compressed = 0;
    std::size_t raw_bytes = 0;
    std::size_t packed_bytes = 0;
    for (animation::AnimClipAsset& clip : clips) {
        const int skin = skin_for(clip, skins);
        if (skin < 0) continue;
        const auto s = static_cast<std::size_t>(skin);
        // The armature's scale turns the metre error bounds into skin units.
        const float scale = s < skin_roots.size()
                                ? std::cbrt(std::abs(glm::determinant(glm::mat3(skin_roots[s]))))
                                : 1.0F;
        const auto stats = animation::compress_clip(clip, skins[s], {},
                                                    scale > 1e-6F && std::isfinite(scale) ? scale : 1.0F);
        if (!stats) {
            std::fprintf(stderr, "[anim] %s: %s\n", label.c_str(), stats.error().message.c_str());
            continue;
        }
        ++compressed;
        raw_bytes += stats->raw_bytes;
        packed_bytes += stats->compressed_bytes;
    }
    if (compressed > 0) {
        std::fprintf(stderr, "[anim] %s: %zu/%zu clips compressed (ACL): %.1f KB -> %.1f KB\n",
                     label.c_str(), compressed, clips.size(),
                     static_cast<double>(raw_bytes) / 1024.0,
                     static_cast<double>(packed_bytes) / 1024.0);
    }
}

} // namespace engine::scene
