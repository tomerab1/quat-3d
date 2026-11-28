#pragma once

// Private to render_settings.cpp: the helpers its descriptor table is built
// with, and the R20 post-stack rows of the table (kept here so the table's
// translation unit stays small).

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>

#include "engine/renderer/render_settings.hpp"

namespace engine::renderer::settings_table {

// Typed reference to member `Field` of group `Group` of a RenderSettings.
template <auto Group, auto Field>
RenderSettingRef field_ref(RenderSettings& s) {
    auto& v = (s.*Group).*Field;
    using V = std::remove_cvref_t<decltype(v)>;
    if constexpr (std::is_same_v<V, bool>) {
        return {.b = &v};
    } else if constexpr (std::is_same_v<V, int>) {
        return {.i = &v};
    } else if constexpr (std::is_same_v<V, std::string>) {
        return {.s = &v};
    } else {
        static_assert(std::is_same_v<V, float>, "settings fields are bool, int, float or string");
        return {.f = &v};
    }
}

using T = RenderSettingType;
using S = RenderSettings;

template <std::size_t A, std::size_t B>
constexpr std::array<RenderSettingDesc, A + B>
concat_fields(const std::array<RenderSettingDesc, A>& a, const std::array<RenderSettingDesc, B>& b) {
    std::array<RenderSettingDesc, A + B> out{};
    for (std::size_t i = 0; i < A; ++i) out[i] = a[i];
    for (std::size_t i = 0; i < B; ++i) out[A + i] = b[i];
    return out;
}

using Dof = DofSettings;
using Mb = MotionBlurSettings;
using Grade = GradingSettings;
using Film = FilmSettings;

// A per-channel lift / gamma / gain row.
template <auto Field>
constexpr RenderSettingDesc grade_channel(std::string_view name, std::string_view label, float min,
                                          float max, float def, std::string_view hint) {
    return RenderSettingDesc{"grading", name, label, T::number, min, max, def, "", hint,
                             &field_ref<&S::grading, Field>};
}

inline constexpr std::array k_post_fields = {
    // ---- dof ----
    RenderSettingDesc{"dof", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 0.0F, "",
                      "Blur what is out of focus (the camera's aperture, focal length and focus "
                      "distance set the blur)",
                      &field_ref<&S::dof, &Dof::enabled>},
    RenderSettingDesc{"dof", "max_radius_px", "max blur radius", T::number, 4.0F, 64.0F, 24.0F,
                      "px", "Largest circle-of-confusion radius (bounds the cost)",
                      &field_ref<&S::dof, &Dof::max_radius_px>},
    RenderSettingDesc{"dof", "rings", "quality", T::integer, 2.0F, 6.0F, 4.0F, "",
                      "Gather rings per field (more = smoother bokeh, slower)",
                      &field_ref<&S::dof, &Dof::rings>},
    // ---- motion_blur ----
    RenderSettingDesc{"motion_blur", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 0.0F, "",
                      "Blur moving things along their motion while the shutter is open",
                      &field_ref<&S::motion_blur, &Mb::enabled>},
    RenderSettingDesc{"motion_blur", "max_radius_px", "max blur radius", T::number, 8.0F, 64.0F,
                      32.0F, "px", "Longest blur radius (also the velocity tile size)",
                      &field_ref<&S::motion_blur, &Mb::max_radius_px>},
    RenderSettingDesc{"motion_blur", "samples", "samples", T::integer, 4.0F, 32.0F, 12.0F, "",
                      "Reconstruction samples per pixel",
                      &field_ref<&S::motion_blur, &Mb::samples>},
    RenderSettingDesc{"motion_blur", "scale", "scale", T::number, 0.0F, 2.0F, 1.0F, "x",
                      "Multiplies the camera's shutter fraction (1 = physical)",
                      &field_ref<&S::motion_blur, &Mb::scale>},
    // ---- tonemap ----
    RenderSettingDesc{"tonemap", "operator", "operator", T::choice, 0.0F,
                      static_cast<float>(tonemap_operator_names.size() - 1), 0.0F, "",
                      "aces (filmic, saturated), agx (Blender 4, natural highlight desaturation), "
                      "pbr_neutral (Khronos, keeps base colours)",
                      &field_ref<&S::tonemap, &TonemapSettings::op>, tonemap_operator_names},
    // ---- grading ----
    RenderSettingDesc{"grading", "temperature_k", "white balance", T::number, 1500.0F, 15000.0F,
                      6500.0F, "K", "Colour temperature rendered as white (lower = cooler image)",
                      &field_ref<&S::grading, &Grade::temperature_k>},
    RenderSettingDesc{"grading", "tint", "tint", T::number, -1.0F, 1.0F, 0.0F, "",
                      "White balance tint: green (-) to magenta (+)",
                      &field_ref<&S::grading, &Grade::tint>},
    RenderSettingDesc{"grading", "contrast", "contrast", T::number, 0.0F, 2.0F, 1.0F, "",
                      "Contrast around mid-grey (in ACEScct)",
                      &field_ref<&S::grading, &Grade::contrast>},
    RenderSettingDesc{"grading", "saturation", "saturation", T::number, 0.0F, 2.0F, 1.0F, "",
                      "Colourfulness (0 = monochrome)",
                      &field_ref<&S::grading, &Grade::saturation>},
    grade_channel<&Grade::lift_r>("lift_r", "lift R", -0.5F, 0.5F, 0.0F, "Shadows offset, red"),
    grade_channel<&Grade::lift_g>("lift_g", "lift G", -0.5F, 0.5F, 0.0F, "Shadows offset, green"),
    grade_channel<&Grade::lift_b>("lift_b", "lift B", -0.5F, 0.5F, 0.0F, "Shadows offset, blue"),
    grade_channel<&Grade::gamma_r>("gamma_r", "gamma R", 0.2F, 5.0F, 1.0F, "Mid-tones, red"),
    grade_channel<&Grade::gamma_g>("gamma_g", "gamma G", 0.2F, 5.0F, 1.0F, "Mid-tones, green"),
    grade_channel<&Grade::gamma_b>("gamma_b", "gamma B", 0.2F, 5.0F, 1.0F, "Mid-tones, blue"),
    grade_channel<&Grade::gain_r>("gain_r", "gain R", 0.0F, 4.0F, 1.0F, "Highlights scale, red"),
    grade_channel<&Grade::gain_g>("gain_g", "gain G", 0.0F, 4.0F, 1.0F, "Highlights scale, green"),
    grade_channel<&Grade::gain_b>("gain_b", "gain B", 0.0F, 4.0F, 1.0F, "Highlights scale, blue"),
    RenderSettingDesc{"grading", "lut", "look LUT", T::path, 0.0F, 0.0F, 0.0F, ".cube",
                      "Project-relative .cube 3D LUT applied after tonemapping (\"\" = none)",
                      &field_ref<&S::grading, &Grade::lut>},
    RenderSettingDesc{"grading", "lut_contribution", "LUT strength", T::number, 0.0F, 1.0F, 1.0F,
                      "", "Blend toward the look LUT's output",
                      &field_ref<&S::grading, &Grade::lut_contribution>},
    // ---- film ----
    RenderSettingDesc{"film", "vignette_natural", "natural vignette", T::number, 0.0F, 1.0F, 0.0F,
                      "", "Lens cos^4 light falloff of the camera's field of view (1 = physical)",
                      &field_ref<&S::film, &Film::vignette_natural>},
    RenderSettingDesc{"film", "vignette_intensity", "vignette", T::number, 0.0F, 1.0F, 0.0F, "",
                      "Artistic darkening toward the corners",
                      &field_ref<&S::film, &Film::vignette_intensity>},
    RenderSettingDesc{"film", "vignette_smoothness", "vignette softness", T::number, 0.05F, 1.0F,
                      0.5F, "", "Width of the artistic vignette's falloff",
                      &field_ref<&S::film, &Film::vignette_smoothness>},
    RenderSettingDesc{"film", "grain_intensity", "film grain", T::number, 0.0F, 1.0F, 0.0F, "",
                      "Film grain strength (after tonemapping, strongest in the mid-tones)",
                      &field_ref<&S::film, &Film::grain_intensity>},
    RenderSettingDesc{"film", "grain_size_px", "grain size", T::number, 0.5F, 4.0F, 1.5F, "px",
                      "Size of a grain", &field_ref<&S::film, &Film::grain_size_px>},
    RenderSettingDesc{"film", "chromatic_aberration", "chromatic aberration", T::number, 0.0F, 1.0F,
                      0.0F, "", "Lateral colour fringing toward the corners",
                      &field_ref<&S::film, &Film::chromatic_aberration>},
};

} // namespace engine::renderer::settings_table
