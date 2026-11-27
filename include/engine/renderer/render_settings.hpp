#pragma once

// RenderSettings — every tunable renderer parameter in one POD (ROADMAP A8).
//
// The application owns one instance; the frame loop reads it each frame and
// hands each pass its group, the editor's Renderer panel edits it, the remote
// API (render.settings.*) reads and writes it, and scene files persist it. All
// of them go through the descriptor table below (render_setting_fields()), so
// a new setting is one struct field plus one table entry: the codec, the
// remote schema and the editor widgets follow from the entry.
//
// Only parameters the renderer implements are listed. The sun is not here: it
// is scene data (a DirectionalLight entity, environment.*).

#include <span>
#include <string_view>

#include "engine/renderer/cloud_settings.hpp"

namespace engine::renderer {

// Histogram auto-exposure (ExposurePass) and the exposure the tonemap applies.
struct ExposureSettings {
    bool  auto_exposure   = true;  // adapt to the measured scene luminance
    float compensation_ev = 0.0F;  // bias in stops (manual mode: exposure = 2^EV)
    float key             = 0.18F; // mid-grey the adapted luminance maps to
    float min_exposure    = 0.05F; // auto exposure multiplier clamp
    float max_exposure    = 16.0F;
    float adaptation_s    = 1.1F;  // exponential adaptation time constant
};

struct BloomSettings {
    bool  enabled   = true;
    float threshold = 1.0F;
    float knee      = 0.6F;
    float intensity = 0.06F;
    float radius    = 1.0F;
};

struct TaaSettings {
    bool  enabled  = true;
    float feedback = 0.9F; // history weight of the resolve
};

// Directional shadow map: an orthographic box around the scene centre.
struct ShadowSettings {
    bool  enabled    = true;
    float extent_m   = 14.0F;   // half-size of the light-space box
    float depth_bias = 0.0008F; // constant depth bias (shadow-map depth units)
    float slope_bias = 0.0025F; // extra bias scaled by (1 - N.L)
    int   pcf_radius = 1;       // (2r+1)^2 filter taps
};

// Image-based ambient + reflections (dynamic IBL probe).
struct IblSettings {
    float intensity = 1.0F;
};

struct RenderSettings {
    ExposureSettings exposure;
    BloomSettings    bloom;
    TaaSettings      taa;
    ShadowSettings   shadows;
    IblSettings      ibl;
    // CloudSettings::time_s is frame-loop state, not a setting: the renderer
    // overwrites it each frame and the table does not list it.
    CloudSettings    clouds;
};

// ---- Descriptor table ----------------------------------------------------------

enum class RenderSettingType { boolean, number, integer };

// Where a field lives in a RenderSettings; exactly one pointer is set,
// matching the field's type.
struct RenderSettingRef {
    bool*  b = nullptr;
    float* f = nullptr;
    int*   i = nullptr;
};

struct RenderSettingGroup {
    std::string_view name;  // JSON key
    std::string_view label; // editor header
    std::string_view hint;  // one line
};

struct RenderSettingDesc {
    std::string_view  group;
    std::string_view  name;  // JSON key
    std::string_view  label; // editor widget label
    RenderSettingType type;
    float             min = 0.0F; // inclusive range (booleans: 0..1)
    float             max = 1.0F;
    float             default_value = 0.0F; // equals the struct default
    std::string_view  unit;                 // "" when unitless
    std::string_view  hint;                 // one line (tooltip / schema)
    RenderSettingRef (*ref)(RenderSettings&) = nullptr;
};

// Groups in display order, and every field grouped in that order.
[[nodiscard]] std::span<const RenderSettingGroup> render_setting_groups();
[[nodiscard]] std::span<const RenderSettingDesc>  render_setting_fields();
[[nodiscard]] const RenderSettingGroup* find_render_setting_group(std::string_view group);
[[nodiscard]] const RenderSettingDesc*  find_render_setting(std::string_view group,
                                                            std::string_view name);

// Value as a float (booleans 0 / 1).
[[nodiscard]] float render_setting_value(const RenderSettings& settings,
                                         const RenderSettingDesc& field);
// Stores `value` (clamped into the field's range; booleans: != 0, integers:
// rounded).
void set_render_setting(RenderSettings& settings, const RenderSettingDesc& field, float value);

// Restores one group's defaults (empty `group`: every group). False for an
// unknown group name.
bool reset_render_settings(RenderSettings& settings, std::string_view group = {});

} // namespace engine::renderer
