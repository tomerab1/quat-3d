#pragma once

// FogVolume — a local volume of fog (ROADMAP R13.3). Placed by the entity's
// Transform: the unit volume ([-1, 1]³ box, or the unit sphere) scaled,
// rotated and moved into the world, so an entity scaled (40, 5, 60) is an
// 80 x 10 x 120 m bank. FogPass voxelises every volume into the froxel grid
// (its density added to the height fog's) and lights it like the rest of
// the medium. POD; the codec and editor read its fields.

#include <cstdint>

#include <glm/glm.hpp>

namespace engine::scene {

enum class FogVolumeShape : std::uint8_t { box, ellipsoid };

struct FogVolume {
    FogVolumeShape shape = FogVolumeShape::box;
    float          density = 0.02F;     // extinction /m inside (visibility ~ 3.9 / density)
    glm::vec3      albedo{0.9F};        // scattering / extinction per channel
    float          edge_fade = 0.25F;   // fraction of the half-extent over which the edges fade
    float          height_falloff = 1.5F; // density e-folds per unit of local height from the bottom
};

} // namespace engine::scene
