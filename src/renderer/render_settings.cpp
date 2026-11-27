#include "engine/renderer/render_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace engine::renderer {

namespace {

// Typed reference to member `Field` of group `Group` of a RenderSettings.
template <auto Group, auto Field>
RenderSettingRef field_ref(RenderSettings& s) {
    auto& v = (s.*Group).*Field;
    using V = std::remove_cvref_t<decltype(v)>;
    if constexpr (std::is_same_v<V, bool>) {
        return {.b = &v};
    } else if constexpr (std::is_same_v<V, int>) {
        return {.i = &v};
    } else {
        static_assert(std::is_same_v<V, float>, "settings fields are bool, int or float");
        return {.f = &v};
    }
}

using T = RenderSettingType;
using S = RenderSettings;

constexpr std::array k_groups = {
    RenderSettingGroup{"exposure", "Exposure", "Auto exposure and exposure compensation"},
    RenderSettingGroup{"bloom", "Bloom", "Glow around bright highlights"},
    RenderSettingGroup{"taa", "Anti-aliasing", "Temporal anti-aliasing"},
    RenderSettingGroup{"upscaler", "Upscaling", "Render resolution and temporal upscaling"},
    RenderSettingGroup{"shadows", "Shadows", "Directional (sun) shadow map"},
    RenderSettingGroup{"ibl", "Environment light", "Image-based ambient light and reflections"},
    RenderSettingGroup{"clouds", "Clouds", "Volumetric cloud layer of the sky"},
};

constexpr std::array k_fields = {
    // ---- exposure ----
    RenderSettingDesc{"exposure", "auto", "auto exposure", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "Adapt to the scene's measured brightness; off = fixed exposure",
                      &field_ref<&S::exposure, &ExposureSettings::auto_exposure>},
    RenderSettingDesc{"exposure", "compensation_ev", "compensation", T::number, -10.0F, 10.0F,
                      0.0F, "EV", "Exposure bias in stops; with auto off, exposure = 2^EV",
                      &field_ref<&S::exposure, &ExposureSettings::compensation_ev>},
    RenderSettingDesc{"exposure", "key", "key (mid-grey)", T::number, 0.01F, 1.0F, 0.18F, "",
                      "Brightness the average scene luminance is mapped to",
                      &field_ref<&S::exposure, &ExposureSettings::key>},
    RenderSettingDesc{"exposure", "min_exposure", "min exposure", T::number, 0.001F, 64.0F, 0.05F,
                      "x", "Lowest auto exposure multiplier (how far bright scenes darken)",
                      &field_ref<&S::exposure, &ExposureSettings::min_exposure>},
    RenderSettingDesc{"exposure", "max_exposure", "max exposure", T::number, 0.001F, 64.0F, 16.0F,
                      "x", "Highest auto exposure multiplier (how far dark scenes brighten)",
                      &field_ref<&S::exposure, &ExposureSettings::max_exposure>},
    RenderSettingDesc{"exposure", "adaptation_s", "adaptation time", T::number, 0.05F, 10.0F, 1.1F,
                      "s", "Time constant of the eye adaptation",
                      &field_ref<&S::exposure, &ExposureSettings::adaptation_s>},
    // ---- bloom ----
    RenderSettingDesc{"bloom", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "Run the bloom pass",
                      &field_ref<&S::bloom, &BloomSettings::enabled>},
    RenderSettingDesc{"bloom", "threshold", "threshold", T::number, 0.0F, 8.0F, 1.0F, "",
                      "HDR luminance above which highlights bloom",
                      &field_ref<&S::bloom, &BloomSettings::threshold>},
    RenderSettingDesc{"bloom", "knee", "knee", T::number, 0.0F, 1.0F, 0.6F, "",
                      "Soft transition width around the threshold",
                      &field_ref<&S::bloom, &BloomSettings::knee>},
    RenderSettingDesc{"bloom", "intensity", "intensity", T::number, 0.0F, 1.0F, 0.06F, "",
                      "Strength of the glow added back to the image",
                      &field_ref<&S::bloom, &BloomSettings::intensity>},
    RenderSettingDesc{"bloom", "radius", "radius", T::number, 0.2F, 2.0F, 1.0F, "",
                      "Spread of the glow",
                      &field_ref<&S::bloom, &BloomSettings::radius>},
    // ---- taa ----
    RenderSettingDesc{"taa", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "Jitter frames and resolve them against the history",
                      &field_ref<&S::taa, &TaaSettings::enabled>},
    RenderSettingDesc{"taa", "feedback", "feedback", T::number, 0.0F, 0.98F, 0.9F, "",
                      "History weight: higher is smoother but ghosts more",
                      &field_ref<&S::taa, &TaaSettings::feedback>},
    // ---- upscaler ----
    RenderSettingDesc{"upscaler", "mode", "mode", T::choice, 0.0F,
                      static_cast<float>(upscaler_mode_names.size() - 1), 0.0F, "",
                      "Render scale per axis: native_aa 1, quality 1/1.5, balanced 1/1.7, "
                      "performance 1/2 (needs TAA on)",
                      &field_ref<&S::upscaler, &UpscalerSettings::mode>, upscaler_mode_names},
    RenderSettingDesc{"upscaler", "render_scale", "scale override", T::number, 0.0F, 1.0F, 0.0F,
                      "x", "Overrides the mode's render scale per axis when above 0 (min 0.25)",
                      &field_ref<&S::upscaler, &UpscalerSettings::render_scale>},
    RenderSettingDesc{"upscaler", "sharpness", "sharpness", T::number, 0.0F, 1.0F, 0.25F, "",
                      "Sharpens the current frame's reconstruction",
                      &field_ref<&S::upscaler, &UpscalerSettings::sharpness>},
    RenderSettingDesc{"upscaler", "backend", "backend", T::choice, 0.0F,
                      static_cast<float>(upscaler_backend_names.size() - 1), 0.0F, "",
                      "taau (built in); fsr2 is not built yet and falls back to taau",
                      &field_ref<&S::upscaler, &UpscalerSettings::backend>,
                      upscaler_backend_names},
    // ---- shadows ----
    RenderSettingDesc{"shadows", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "Render and sample the sun's shadow map",
                      &field_ref<&S::shadows, &ShadowSettings::enabled>},
    RenderSettingDesc{"shadows", "extent_m", "extent", T::number, 2.0F, 500.0F, 14.0F, "m",
                      "Half-size of the shadowed box around the world origin",
                      &field_ref<&S::shadows, &ShadowSettings::extent_m>},
    RenderSettingDesc{"shadows", "depth_bias", "depth bias", T::number, 0.0F, 0.05F, 0.0008F, "",
                      "Constant depth bias against shadow acne",
                      &field_ref<&S::shadows, &ShadowSettings::depth_bias>},
    RenderSettingDesc{"shadows", "slope_bias", "slope bias", T::number, 0.0F, 0.05F, 0.0025F, "",
                      "Extra bias on surfaces facing away from the sun",
                      &field_ref<&S::shadows, &ShadowSettings::slope_bias>},
    RenderSettingDesc{"shadows", "pcf_radius", "filter radius", T::integer, 0.0F, 3.0F, 1.0F,
                      "texels", "PCF kernel radius: softer edges, (2r+1)^2 samples",
                      &field_ref<&S::shadows, &ShadowSettings::pcf_radius>},
    // ---- ibl ----
    RenderSettingDesc{"ibl", "intensity", "intensity", T::number, 0.0F, 4.0F, 1.0F, "",
                      "Scales image-based ambient light and reflections",
                      &field_ref<&S::ibl, &IblSettings::intensity>},
    // ---- clouds ----
    RenderSettingDesc{"clouds", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "Draw the volumetric cloud layer",
                      &field_ref<&S::clouds, &CloudSettings::enabled>},
    RenderSettingDesc{"clouds", "coverage", "coverage", T::number, 0.0F, 1.0F, 0.42F, "",
                      "Fraction of the sky covered",
                      &field_ref<&S::clouds, &CloudSettings::coverage>},
    RenderSettingDesc{"clouds", "density", "density", T::number, 0.0F, 128.0F, 24.0F, "/km",
                      "Extinction at full density",
                      &field_ref<&S::clouds, &CloudSettings::density>},
    RenderSettingDesc{"clouds", "bottom_km", "altitude", T::number, 0.2F, 15.0F, 1.5F, "km",
                      "Cloud base above the ground",
                      &field_ref<&S::clouds, &CloudSettings::bottom_km>},
    RenderSettingDesc{"clouds", "thickness_km", "thickness", T::number, 0.2F, 10.0F, 2.5F, "km",
                      "Thickness of the cloud layer",
                      &field_ref<&S::clouds, &CloudSettings::thickness_km>},
    RenderSettingDesc{"clouds", "size_km", "puff size", T::number, 1.0F, 80.0F, 10.0F, "km",
                      "Base size of the cloud shapes",
                      &field_ref<&S::clouds, &CloudSettings::size_km>},
    RenderSettingDesc{"clouds", "detail", "detail", T::number, 0.0F, 1.0F, 0.4F, "",
                      "Edge erosion strength",
                      &field_ref<&S::clouds, &CloudSettings::detail>},
    RenderSettingDesc{"clouds", "wind_speed", "wind speed", T::number, 0.0F, 100.0F, 12.0F, "m/s",
                      "Drift speed",
                      &field_ref<&S::clouds, &CloudSettings::wind_speed>},
    RenderSettingDesc{"clouds", "wind_dir_deg", "wind direction", T::number, -180.0F, 180.0F,
                      35.0F, "deg", "Heading the clouds drift toward (from +X toward +Z)",
                      &field_ref<&S::clouds, &CloudSettings::wind_dir_deg>},
};

} // namespace

std::span<const RenderSettingGroup> render_setting_groups() { return k_groups; }

std::span<const RenderSettingDesc> render_setting_fields() { return k_fields; }

const RenderSettingGroup* find_render_setting_group(std::string_view group) {
    for (const RenderSettingGroup& g : k_groups) {
        if (g.name == group) return &g;
    }
    return nullptr;
}

const RenderSettingDesc* find_render_setting(std::string_view group, std::string_view name) {
    for (const RenderSettingDesc& d : k_fields) {
        if (d.group == group && d.name == name) return &d;
    }
    return nullptr;
}

float render_setting_value(const RenderSettings& settings, const RenderSettingDesc& field) {
    // ref() only forms pointers into the copy; reading through them is const.
    RenderSettings copy = settings;
    const RenderSettingRef ref = field.ref(copy);
    if (ref.b != nullptr) return *ref.b ? 1.0F : 0.0F;
    if (ref.i != nullptr) return static_cast<float>(*ref.i);
    return *ref.f;
}

std::string_view render_setting_choice_name(const RenderSettingDesc& field, float value) {
    if (field.type != RenderSettingType::choice || !std::isfinite(value)) return {};
    const long i = std::lround(value);
    if (i < 0 || static_cast<std::size_t>(i) >= field.choices.size()) return {};
    return field.choices[static_cast<std::size_t>(i)];
}

std::optional<int> render_setting_choice_index(const RenderSettingDesc& field,
                                               std::string_view name) {
    for (std::size_t i = 0; i < field.choices.size(); ++i) {
        if (field.choices[i] == name) return static_cast<int>(i);
    }
    return std::nullopt;
}

void set_render_setting(RenderSettings& settings, const RenderSettingDesc& field, float value) {
    const RenderSettingRef ref = field.ref(settings);
    if (ref.b != nullptr) {
        *ref.b = value != 0.0F;
        return;
    }
    const float v = std::isfinite(value) ? std::clamp(value, field.min, field.max) : field.default_value;
    if (ref.i != nullptr) {
        *ref.i = static_cast<int>(std::lround(v));
    } else {
        *ref.f = v;
    }
}

bool reset_render_settings(RenderSettings& settings, std::string_view group) {
    if (group.empty()) {
        const float time = settings.clouds.time_s;
        settings = RenderSettings{};
        settings.clouds.time_s = time;
        return true;
    }
    if (find_render_setting_group(group) == nullptr) return false;
    for (const RenderSettingDesc& d : k_fields) {
        if (d.group == group) set_render_setting(settings, d, d.default_value);
    }
    return true;
}

} // namespace engine::renderer
