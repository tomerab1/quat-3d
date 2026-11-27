#pragma once

// Clip planes of the view camera derived from what the scene contains
// (ROADMAP A9, issue #37). The editor's fly camera used fixed 0.1 / 1000 m
// planes, so streamed terrain past 1 km was cut off. Here the far plane
// reaches the farthest point of the scene's drawables and of the terrain
// (the streaming window around the camera), and the near plane follows the
// far one so the depth range stays usable.
//
// Depth is standard Z in D32_SFLOAT ([0, 1], cleared to 1, LESS): its
// resolution at distance z is about z² · 2⁻²⁴ / near, so the far/near ratio is
// bounded (k_max_far_near_ratio) and the near plane grows only once the far
// plane passes k_max_far_near_ratio · k_min_near (5 km). Explicit near / far
// overrides (remote `camera.set`) are honoured as given.

#include <optional>

#include <glm/glm.hpp>

#include "engine/asset/mesh_asset.hpp" // Aabb

namespace engine::scene {

struct ClipRange {
    float near_z = 0.1F;
    float far_z = 1000.0F;
};

// Limits of the derived range (metres).
inline constexpr float k_min_near = 0.1F;
inline constexpr float k_max_near = 2.0F;
inline constexpr float k_min_far = 500.0F;
inline constexpr float k_max_far = 200'000.0F;
inline constexpr float k_max_far_near_ratio = 50'000.0F;

// What the camera should be able to see, in world space.
struct ViewExtent {
    std::optional<asset::Aabb> drawables; // scene_draw_bounds()
    std::optional<asset::Aabb> terrain;   // loaded tile / streaming window
};

// The far plane encloses every corner of the extent seen from `eye` (with a
// margin), within [k_min_far, k_max_far]; the near plane is far /
// k_max_far_near_ratio within [k_min_near, k_max_near].
[[nodiscard]] ClipRange derive_clip_range(const glm::vec3& eye, const ViewExtent& extent);

// The near plane the derivation pairs with `far_z`.
[[nodiscard]] float near_for_far(float far_z);

// Applies explicit overrides to a derived range. An overridden far keeps a
// derived near paired with it (near_for_far); an overridden near pushes the
// far plane out if needed so far > near.
[[nodiscard]] ClipRange apply_clip_overrides(const ClipRange& derived,
                                             std::optional<float> near_override,
                                             std::optional<float> far_override);

// Extent of terrain streaming around a camera: the (2·radius + 1)² window of
// `tile_size` tiles centred on the tile under `eye`, with tile (0, 0) centred
// on `anchor`; heights span [anchor.y + min_height, anchor.y + max_height].
[[nodiscard]] asset::Aabb streaming_window_bounds(const glm::vec3& eye, const glm::vec3& anchor,
                                                  float tile_size, int radius, float min_height,
                                                  float max_height);

} // namespace engine::scene
