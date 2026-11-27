#pragma once

// MotionHistory — last frame's transforms and joint palettes, for the velocity
// buffer (ROADMAP R11; contract in velocity.hpp).
//
// Double-buffered by frame: begin_frame() makes everything recorded during
// the previous frame the "previous" set and starts an empty current set.
// Within a frame, advance(key, current) records `current` and returns the
// previous frame's value for `key` — or `current` itself when the key was not
// seen last frame (new object: camera-only velocity). Calling advance twice
// for one key in a frame is harmless (both calls see the same previous value).
// Keys not recorded during a frame drop out of the history on the next
// begin_frame, so despawned objects cost nothing.
//
// Keys are opaque 64-bit ids chosen by the caller (the app uses the entt
// entity id, which includes its version, so a recycled id never inherits a
// stale transform). Transforms and palettes live in separate key spaces.
// CPU-only (one per view, shared by the frames in flight).

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace engine::renderer {

class MotionHistory {
public:
    void begin_frame();

    // Drop all history (camera cut, scene load): the next frame is camera-only.
    void reset();

    [[nodiscard]] glm::mat4 advance(std::uint64_t key, const glm::mat4& current);

    // Joint palettes (skinning matrices). The returned span stays valid until
    // the next begin_frame / reset; it is `current` itself when the key has no
    // previous palette of the same size.
    [[nodiscard]] std::span<const glm::mat4> advance_palette(std::uint64_t key,
                                                             std::span<const glm::mat4> current);

    [[nodiscard]] std::size_t tracked_transforms() const { return current_.size(); }
    [[nodiscard]] std::size_t tracked_palettes() const { return current_palettes_.size(); }

private:
    std::unordered_map<std::uint64_t, glm::mat4>              previous_;
    std::unordered_map<std::uint64_t, glm::mat4>              current_;
    std::unordered_map<std::uint64_t, std::vector<glm::mat4>> previous_palettes_;
    std::unordered_map<std::uint64_t, std::vector<glm::mat4>> current_palettes_;
};

} // namespace engine::renderer
