#pragma once

#include <cstdint>

namespace engine::rhi {

// What the device can do with sampled 2D textures, for asset loads that run
// without a Device. `bc` / `astc_ldr`: block-compressed families it samples
// (the features are enabled at device creation whenever offered);
// `prefer_astc` marks GPUs whose native format is ASTC (Apple), where it is
// chosen over BC when both exist. `max_dimension_2d` is
// maxImageDimension2D, 0 when unknown. All defaults = uncompressed RGBA8 only.
struct TextureCaps {
    bool          bc               = false;
    bool          astc_ldr         = false;
    bool          prefer_astc      = false;
    std::uint32_t max_dimension_2d = 0;
};

} // namespace engine::rhi
