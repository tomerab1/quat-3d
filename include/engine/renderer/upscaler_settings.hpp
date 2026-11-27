#pragma once

// User-facing upscaler settings (ROADMAP R11): a plain struct the editor, the
// remote API and MCP edit (A8 owns the schema), plus the mode -> render-scale
// rules. The Upscaler interface itself is in upscaler.hpp.

#include <cstdint>
#include <optional>
#include <string_view>

#include <vulkan/vulkan.h>

namespace engine::renderer {

enum class UpscalerBackend : std::uint8_t {
    taau, // the engine's temporal upsampler (taa_pass.hpp)
    fsr2, // AMD FidelityFX FSR 2 (not built yet: docs/upscaling.md)
};

// Quality modes and their render scale per axis (AMD's FSR 2 ratios).
enum class UpscalerMode : std::uint8_t {
    native_aa,   // 1.0: anti-aliasing only
    quality,     // 1 / 1.5
    balanced,    // 1 / 1.7
    performance, // 1 / 2
};

// Out-of-range values are clamped where used.
struct UpscalerSettings {
    UpscalerBackend backend = UpscalerBackend::taau;
    UpscalerMode    mode = UpscalerMode::native_aa;
    // > 0 overrides the mode's render scale (per axis, clamped to [0.25, 1]).
    float render_scale = 0.0F;
    // Sharpening of the current frame's reconstruction, [0, 1].
    float sharpness = 0.25F;
};

[[nodiscard]] float upscaler_mode_scale(UpscalerMode mode);
// The render scale `settings` select (override or mode), clamped.
[[nodiscard]] float upscaler_render_scale(const UpscalerSettings& settings);
// Render extent for `display`: each axis scaled and rounded, at least 1 pixel.
[[nodiscard]] VkExtent2D upscaler_render_extent(VkExtent2D display,
                                                const UpscalerSettings& settings);

[[nodiscard]] std::string_view to_string(UpscalerMode mode);
[[nodiscard]] std::string_view to_string(UpscalerBackend backend);
[[nodiscard]] std::optional<UpscalerMode> parse_upscaler_mode(std::string_view name);
[[nodiscard]] std::optional<UpscalerBackend> parse_upscaler_backend(std::string_view name);

} // namespace engine::renderer
