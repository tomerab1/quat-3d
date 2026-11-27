#pragma once

namespace engine::rhi {

// Block-compressed texture families the device can sample (the features are
// enabled at device creation whenever offered). `prefer_astc` marks GPUs whose
// native format is ASTC (Apple), where it is chosen over BC when both exist.
// All false = uncompressed RGBA8 only.
struct TextureCompressionSupport {
    bool bc          = false;
    bool astc_ldr    = false;
    bool prefer_astc = false;
};

} // namespace engine::rhi
