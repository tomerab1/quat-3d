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

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/post_settings.hpp"
#include "engine/renderer/reflection_settings.hpp"
#include "engine/renderer/shadow_ao_settings.hpp"
#include "engine/renderer/upscaler_settings.hpp"

namespace engine::renderer {

// Histogram auto-exposure (ExposurePass) and the exposure the tonemap applies.
struct ExposureSettings {
    bool  auto_exposure   = true;  // adapt to the measured scene luminance
    // R20: exposure from the active camera's aperture / shutter / ISO (EV100);
    // takes precedence over auto_exposure. The compensation still applies.
    bool  physical        = false;
    float compensation_ev = 0.0F;  // bias in stops (manual mode: exposure = 2^EV)
    float key             = 0.18F; // mid-grey the adapted luminance maps to
    // Auto exposure range in EV100 (R22.2.1): the scenes it adapts to, from
    // a moonless night (~-11) to sunlit snow (~16).
    float min_ev100       = -14.0F;
    float max_ev100       = 20.0F;
    // Krawczyk et al. 2005's adaptive key: dim scenes are shown dimmer than
    // `key` (photometry.hpp); off = always `key`.
    bool  adaptive_key    = true;
    float adaptation_s    = 1.1F;  // exponential adaptation time constant
};

struct BloomSettings {
    bool  enabled   = true;
    float threshold = 1.0F;
    float knee      = 0.6F;
    float intensity = 0.06F;
    float radius    = 1.0F;
};

// The temporal resolve (TAAU, R11). Off: no jitter, every frame resolves
// without history and the scene renders at native resolution (the upscaler
// group's scale needs the temporal resolve). `feedback` is the history weight
// a pixel converges to when a sample lands on it every frame; TAAU caps a
// pixel's accumulated sample weight at 1 / (1 - feedback).
struct TaaSettings {
    bool  enabled  = true;
    float feedback = 0.9F;
};

// Image-based ambient + reflections (dynamic IBL probe or HDRI). R21:
// multiple-scattering GGX energy compensation and horizon occlusion of
// normal-mapped reflections.
struct IblSettings {
    float intensity = 1.0F;
    bool  multiscatter = true;
    bool  horizon_occlusion = true;
};

// The night sky over the world clock (R22.2.1).
struct NightSkySettings {
    bool  stars = true;         // the bright star catalogue
    float scintillation = 1.0F; // twinkle strength (0 = steady)
};

struct RenderSettings {
    ExposureSettings exposure;
    BloomSettings    bloom;
    TaaSettings      taa;
    UpscalerSettings upscaler; // R11: render scale mode, sharpness, backend
    CsmSettings      shadows;         // cascaded sun shadows (R12)
    GtaoSettings     ao;              // ground-truth ambient occlusion (R12)
    ContactShadowSettings contact_shadows; // screen-space contact shadows (R12)
    IblSettings      ibl;
    EnvironmentSettings environment; // R21: sky source (procedural / HDRI)
    SsrSettings      ssr;            // R21: screen-space reflections
    // CloudSettings::time_s is frame-loop state, not a setting: the renderer
    // overwrites it each frame and the table does not list it.
    CloudSettings    clouds;
    NightSkySettings night_sky; // R22.2.1
    // R20: the cinematic post stack (post_settings.hpp).
    DofSettings        dof;
    MotionBlurSettings motion_blur;
    TonemapSettings    tonemap;
    GradingSettings    grading;
    FilmSettings       film;
};

// ---- Descriptor table ----------------------------------------------------------

// `choice`: an int field holding an index into RenderSettingDesc::choices,
// encoded by name in JSON (an enum such as the upscaler mode). `path`: a
// std::string holding a project-relative file path ("" = none) whose
// required extension is the descriptor's `unit`; the codec checks its form
// (check_render_setting_path), the consumer loads it as untrusted data.
enum class RenderSettingType { boolean, number, integer, choice, path };

// Where a field lives in a RenderSettings; exactly one pointer is set,
// matching the field's type.
struct RenderSettingRef {
    bool*        b = nullptr;
    float*       f = nullptr;
    int*         i = nullptr;
    std::string* s = nullptr;
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
    std::span<const std::string_view> choices{}; // `choice` fields: names by index
};

// Name of a `choice` field's value (empty for other types or out of range),
// and the index of `name` among its choices.
[[nodiscard]] std::string_view render_setting_choice_name(const RenderSettingDesc& field,
                                                          float value);
[[nodiscard]] std::optional<int> render_setting_choice_index(const RenderSettingDesc& field,
                                                             std::string_view name);

// A key the table no longer lists but older scene files and clients may
// still send: accepted and ignored (never written; listed in the schema as
// deprecated with `note`).
struct DeprecatedRenderSetting {
    std::string_view group;
    std::string_view name;
    std::string_view note;
};

// Groups in display order, and every field grouped in that order.
[[nodiscard]] std::span<const RenderSettingGroup> render_setting_groups();
[[nodiscard]] std::span<const RenderSettingDesc>  render_setting_fields();
[[nodiscard]] const RenderSettingGroup* find_render_setting_group(std::string_view group);
[[nodiscard]] const RenderSettingDesc*  find_render_setting(std::string_view group,
                                                            std::string_view name);
[[nodiscard]] std::span<const DeprecatedRenderSetting> deprecated_render_settings();
[[nodiscard]] const DeprecatedRenderSetting* find_deprecated_render_setting(
    std::string_view group, std::string_view name);

// Value as a float (booleans 0 / 1; `path` fields: 0).
[[nodiscard]] float render_setting_value(const RenderSettings& settings,
                                         const RenderSettingDesc& field);
// Stores `value` (clamped into the field's range; booleans: != 0, integers:
// rounded). No effect on `path` fields.
void set_render_setting(RenderSettings& settings, const RenderSettingDesc& field, float value);

// Text of a `path` field ("" for other types).
[[nodiscard]] std::string render_setting_text(const RenderSettings& settings,
                                              const RenderSettingDesc& field);
// Stores the text of a `path` field after check_render_setting_path; false
// (unchanged) when it does not pass or the field is not a path.
bool set_render_setting_text(RenderSettings& settings, const RenderSettingDesc& field,
                             std::string_view text);
// Why `text` is not an acceptable value of the `path` field `field` (empty
// when it is): at most 256 bytes of printable characters, relative, no `..`
// segment, ending in the field's extension. "" (none) is always acceptable.
[[nodiscard]] std::string check_render_setting_path(const RenderSettingDesc& field,
                                                    std::string_view text);

// Restores one group's defaults (empty `group`: every group). False for an
// unknown group name.
bool reset_render_settings(RenderSettings& settings, std::string_view group = {});

} // namespace engine::renderer
