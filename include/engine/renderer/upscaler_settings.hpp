#pragma once

// User-facing upscaler settings (ROADMAP R11) and the mode -> render-scale
// rules. The settings are the `upscaler` group of A8's RenderSettings
// (render_settings.hpp): its descriptor table drives the editor panel, the
// remote render.settings.* API and scene files. The Upscaler interface itself
// is in upscaler.hpp.

#include <array>
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
// `automatic` (the default, "auto" in JSON) picks per display when the frame
// renders: `quality` on a high-density display (display scale >= 1.5: a
// Retina Mac, a 4K monitor at 150-200 %), where native resolution costs
// 2.25-4x the pixels for little visible gain, else `native_aa`. It is stored
// as `auto`, so a scene saved on one machine still adapts on another.
enum class UpscalerMode : std::uint8_t {
    automatic,   // quality on high-density displays, else native_aa
    native_aa,   // 1.0: anti-aliasing only
    quality,     // 1 / 1.5
    balanced,    // 1 / 1.7
    performance, // 1 / 2
};

// Names in enum order: the descriptor table's choices and the JSON values.
inline constexpr std::array<std::string_view, 5> upscaler_mode_names{
    "auto", "native_aa", "quality", "balanced", "performance"};
inline constexpr std::array<std::string_view, 2> upscaler_backend_names{"taau", "fsr2"};

// A plain struct of the descriptor table's field types (A8): the enums are
// stored as their index; read them through upscaler_mode() /
// upscaler_backend(), which clamp. Out-of-range values are clamped where used.
struct UpscalerSettings {
    int   mode = static_cast<int>(UpscalerMode::automatic);
    // > 0 overrides the mode's render scale (per axis, clamped to [0.25, 1]).
    float render_scale = 0.0F;
    // Sharpening of the current frame's reconstruction, [0, 1].
    float sharpness = 0.25F;
    int   backend = static_cast<int>(UpscalerBackend::taau);
};

[[nodiscard]] UpscalerMode    upscaler_mode(const UpscalerSettings& settings);
[[nodiscard]] UpscalerBackend upscaler_backend(const UpscalerSettings& settings);

// The concrete mode `mode` means on a display of `display_scale`
// (SDL_GetWindowDisplayScale): `automatic` resolves, the others are returned
// as they are.
[[nodiscard]] UpscalerMode resolve_upscaler_mode(UpscalerMode mode, float display_scale);
// `settings` with an `automatic` mode resolved for `display_scale`.
[[nodiscard]] UpscalerSettings resolve_upscaler_settings(UpscalerSettings settings,
                                                         float display_scale);

// The render scale of a concrete mode (an unresolved `automatic` counts as
// native).
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
