#include "engine/renderer/render_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <type_traits>

#include "render_settings_fields.hpp"

namespace engine::renderer {

namespace {

using namespace settings_table; // field_ref, T, S
using Csm = CsmSettings;
using Gtao = GtaoSettings;
using Contact = ContactShadowSettings;
using Env = EnvironmentSettings;
using Ssr = SsrSettings;

constexpr std::array k_groups = {
    RenderSettingGroup{"exposure", "Exposure", "Auto exposure and exposure compensation"},
    RenderSettingGroup{"bloom", "Bloom", "Glow around bright highlights"},
    RenderSettingGroup{"taa", "Anti-aliasing", "Temporal anti-aliasing"},
    RenderSettingGroup{"upscaler", "Upscaling", "Render resolution and temporal upscaling"},
    RenderSettingGroup{"shadows", "Shadows", "Cascaded sun shadow maps"},
    RenderSettingGroup{"ao", "Ambient occlusion", "GTAO with bent normals"},
    RenderSettingGroup{"contact_shadows", "Contact shadows",
                       "Screen-space sun shadows for small detail"},
    RenderSettingGroup{"ibl", "Environment light", "Image-based ambient light and reflections"},
    RenderSettingGroup{"environment", "Sky & HDRI",
                       "Procedural sky or an HDRI panorama for background, ambient and reflections"},
    RenderSettingGroup{"ssr", "Reflections", "Screen-space reflections over the environment"},
    RenderSettingGroup{"clouds", "Clouds", "Volumetric cloud layer of the sky"},
    RenderSettingGroup{"dof", "Depth of field", "Bokeh blur from the camera's lens and focus"},
    RenderSettingGroup{"motion_blur", "Motion blur", "Blur along motion over the camera's shutter"},
    RenderSettingGroup{"tonemap", "Tonemapping", "HDR to display mapping"},
    RenderSettingGroup{"grading", "Colour grading",
                       "White balance, contrast, lift / gamma / gain and look LUT"},
    RenderSettingGroup{"film", "Lens and film", "Vignette, film grain and chromatic aberration"},
};

constexpr std::array k_core_fields = {
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
    RenderSettingDesc{"exposure", "physical", "physical camera", T::boolean, 0.0F, 1.0F, 0.0F, "",
                      "Expose from the camera's aperture, shutter and ISO (EV100); overrides auto",
                      &field_ref<&S::exposure, &ExposureSettings::physical>},
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
    // ---- shadows (cascaded sun shadow maps) ----
    RenderSettingDesc{"shadows", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Render the sun's cascaded shadow maps; off skips the shadow passes",
                      &field_ref<&S::shadows, &Csm::enabled>},
    RenderSettingDesc{"shadows", "max_distance", "distance", T::number, Csm::k_max_distance.min,
                      Csm::k_max_distance.max, Csm{}.max_distance, "m",
                      "Shadows end this far from the camera",
                      &field_ref<&S::shadows, &Csm::max_distance>},
    RenderSettingDesc{"shadows", "cascades", "cascades", T::integer,
                      static_cast<float>(Csm::k_cascade_count.min),
                      static_cast<float>(Csm::k_cascade_count.max),
                      static_cast<float>(Csm{}.cascade_count), "",
                      "Number of shadow cascades",
                      &field_ref<&S::shadows, &Csm::cascade_count>},
    RenderSettingDesc{"shadows", "split_lambda", "split lambda", T::number, Csm::k_split_lambda.min,
                      Csm::k_split_lambda.max, Csm{}.split_lambda, "",
                      "Cascade split spacing: 0 linear, 1 logarithmic",
                      &field_ref<&S::shadows, &Csm::split_lambda>},
    RenderSettingDesc{"shadows", "fit_to_depth", "fit to depth", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Fit the cascades to this frame's depth range on the GPU (SDSM)",
                      &field_ref<&S::shadows, &Csm::gpu_depth_bounds>},
    RenderSettingDesc{"shadows", "blend_band", "blend band", T::number, Csm::k_blend_band.min,
                      Csm::k_blend_band.max, Csm{}.blend_band, "",
                      "Fraction of each cascade cross-faded into the next",
                      &field_ref<&S::shadows, &Csm::blend_band>},
    RenderSettingDesc{"shadows", "pcf_radius", "filter radius", T::number, Csm::k_pcf_radius.min,
                      Csm::k_pcf_radius.max, Csm{}.pcf_radius, "texels",
                      "PCF filter radius: softer edges",
                      &field_ref<&S::shadows, &Csm::pcf_radius>},
    RenderSettingDesc{"shadows", "sun_size_deg", "sun size", T::number, Csm::k_light_size_deg.min,
                      Csm::k_light_size_deg.max, Csm{}.light_size_deg, "deg",
                      "Sun angular diameter for contact-hardening penumbrae; 0 = fixed filter",
                      &field_ref<&S::shadows, &Csm::light_size_deg>},
    RenderSettingDesc{"shadows", "normal_bias_texels", "normal bias",
                      T::number, Csm::k_normal_bias.min,
                      Csm::k_normal_bias.max, Csm{}.normal_bias, "texels",
                      "Receiver offset along the surface normal (against acne)",
                      &field_ref<&S::shadows, &Csm::normal_bias>},
    RenderSettingDesc{"shadows", "depth_bias_texels", "depth bias",
                      T::number, Csm::k_depth_bias.min,
                      Csm::k_depth_bias.max, Csm{}.depth_bias, "texels",
                      "Receiver offset toward the sun (against acne)",
                      &field_ref<&S::shadows, &Csm::depth_bias>},
    RenderSettingDesc{"shadows", "caster_distance", "caster reach",
                      T::number, Csm::k_caster_distance.min,
                      Csm::k_caster_distance.max, Csm{}.caster_distance, "m",
                      "How far toward the sun shadow casters are captured",
                      &field_ref<&S::shadows, &Csm::caster_distance>},
    RenderSettingDesc{"shadows", "debug_cascades", "show cascades", T::boolean, 0.0F, 1.0F, 0.0F,
                      "", "Tint each pixel by the cascade that shadows it",
                      &field_ref<&S::shadows, &Csm::debug_cascades>},
    // ---- ao (GTAO) ----
    RenderSettingDesc{"ao", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Ground-truth ambient occlusion (GTAO) on the ambient light",
                      &field_ref<&S::ao, &Gtao::enabled>},
    RenderSettingDesc{"ao", "radius", "radius", T::number, Gtao::k_radius.min,
                      Gtao::k_radius.max, Gtao{}.radius, "m",
                      "World-space reach of the occlusion search",
                      &field_ref<&S::ao, &Gtao::radius>},
    RenderSettingDesc{"ao", "strength", "strength", T::number, Gtao::k_final_value_power.min,
                      Gtao::k_final_value_power.max, Gtao{}.final_value_power, "",
                      "Exponent on the visibility: higher darkens more",
                      &field_ref<&S::ao, &Gtao::final_value_power>},
    RenderSettingDesc{"ao", "falloff_range", "falloff", T::number, Gtao::k_falloff_range.min,
                      Gtao::k_falloff_range.max, Gtao{}.falloff_range, "",
                      "Fraction of the radius over which occluders fade out",
                      &field_ref<&S::ao, &Gtao::falloff_range>},
    RenderSettingDesc{"ao", "sample_distribution_power", "sample distribution",
                      T::number, Gtao::k_sample_distribution_power.min,
                      Gtao::k_sample_distribution_power.max, Gtao{}.sample_distribution_power, "",
                      "Above 1 packs samples near the pixel",
                      &field_ref<&S::ao, &Gtao::sample_distribution_power>},
    RenderSettingDesc{"ao", "thin_occluder_compensation", "thin occluders",
                      T::number, Gtao::k_thin_occluder_compensation.min,
                      Gtao::k_thin_occluder_compensation.max, Gtao{}.thin_occluder_compensation, "",
                      "Lets light pass behind thin objects",
                      &field_ref<&S::ao, &Gtao::thin_occluder_compensation>},
    RenderSettingDesc{"ao", "depth_mip_offset", "depth mip offset",
                      T::number, Gtao::k_depth_mip_offset.min,
                      Gtao::k_depth_mip_offset.max, Gtao{}.depth_mip_offset, "",
                      "Hi-Z mip bias for distant samples (lower = sharper)",
                      &field_ref<&S::ao, &Gtao::depth_mip_offset>},
    RenderSettingDesc{"ao", "slices", "slices", T::integer,
                      static_cast<float>(Gtao::k_slice_count.min),
                      static_cast<float>(Gtao::k_slice_count.max),
                      static_cast<float>(Gtao{}.slice_count), "",
                      "Search directions per pixel (quality vs cost)",
                      &field_ref<&S::ao, &Gtao::slice_count>},
    RenderSettingDesc{"ao", "steps_per_slice", "steps per slice", T::integer,
                      static_cast<float>(Gtao::k_steps_per_slice.min),
                      static_cast<float>(Gtao::k_steps_per_slice.max),
                      static_cast<float>(Gtao{}.steps_per_slice), "",
                      "Depth samples per direction and side",
                      &field_ref<&S::ao, &Gtao::steps_per_slice>},
    RenderSettingDesc{"ao", "denoise_passes", "denoise passes", T::integer,
                      static_cast<float>(Gtao::k_denoise_passes.min),
                      static_cast<float>(Gtao::k_denoise_passes.max),
                      static_cast<float>(Gtao{}.denoise_passes), "",
                      "Edge-aware blur passes (0 = raw; TAA still accumulates)",
                      &field_ref<&S::ao, &Gtao::denoise_passes>},
    RenderSettingDesc{"ao", "denoise_blur_beta", "denoise blur",
                      T::number, Gtao::k_denoise_blur_beta.min,
                      Gtao::k_denoise_blur_beta.max, Gtao{}.denoise_blur_beta, "",
                      "Centre weight of the denoiser: higher keeps more detail",
                      &field_ref<&S::ao, &Gtao::denoise_blur_beta>},
    RenderSettingDesc{"ao", "bent_normals", "bent normals", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Look up ambient light along the least-occluded direction",
                      &field_ref<&S::ao, &Gtao::bent_normals>},
    RenderSettingDesc{"ao", "specular_occlusion", "specular occlusion",
                      T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Occlude environment reflections in crevices",
                      &field_ref<&S::ao, &Gtao::specular_occlusion>},
    RenderSettingDesc{"ao", "multi_bounce", "multi-bounce", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Brighten occlusion on light surfaces (interreflection)",
                      &field_ref<&S::ao, &Gtao::multi_bounce>},
    // ---- contact_shadows ----
    RenderSettingDesc{"contact_shadows", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Short screen-space shadow rays toward the sun for small detail",
                      &field_ref<&S::contact_shadows, &Contact::enabled>},
    RenderSettingDesc{"contact_shadows", "length", "length", T::number, Contact::k_length.min,
                      Contact::k_length.max, Contact{}.length, "m",
                      "Ray length toward the sun",
                      &field_ref<&S::contact_shadows, &Contact::length>},
    RenderSettingDesc{"contact_shadows", "steps", "steps", T::integer,
                      static_cast<float>(Contact::k_steps.min),
                      static_cast<float>(Contact::k_steps.max),
                      static_cast<float>(Contact{}.steps), "",
                      "Depth samples along the ray",
                      &field_ref<&S::contact_shadows, &Contact::steps>},
    RenderSettingDesc{"contact_shadows", "thickness", "thickness",
                      T::number, Contact::k_thickness.min,
                      Contact::k_thickness.max, Contact{}.thickness, "m",
                      "How far behind the depth buffer a hit still counts",
                      &field_ref<&S::contact_shadows, &Contact::thickness>},
    RenderSettingDesc{"contact_shadows", "intensity", "intensity",
                      T::number, Contact::k_intensity.min,
                      Contact::k_intensity.max, Contact{}.intensity, "",
                      "0 = no effect, 1 = full shadow on a hit",
                      &field_ref<&S::contact_shadows, &Contact::intensity>},
    // ---- ibl ----
    RenderSettingDesc{"ibl", "intensity", "intensity", T::number, 0.0F, 4.0F, 1.0F, "",
                      "Scales image-based ambient light and reflections",
                      &field_ref<&S::ibl, &IblSettings::intensity>},
    RenderSettingDesc{"ibl", "multiscatter", "multiple scattering", T::boolean, 0.0F, 1.0F, 1.0F,
                      "", "Restore the energy rough metals lose to single-scattering GGX",
                      &field_ref<&S::ibl, &IblSettings::multiscatter>},
    RenderSettingDesc{"ibl", "horizon_occlusion", "horizon occlusion", T::boolean, 0.0F, 1.0F,
                      1.0F, "", "Fade normal-mapped reflections that dip below the surface",
                      &field_ref<&S::ibl, &IblSettings::horizon_occlusion>},
    // ---- environment (R21) ----
    RenderSettingDesc{"environment", "source", "sky", T::choice, 0.0F,
                      static_cast<float>(sky_source_names.size() - 1), 0.0F, "",
                      "procedural (atmosphere) or hdri (needs environment.set hdri.path)",
                      &field_ref<&S::environment, &Env::source>, sky_source_names},
    RenderSettingDesc{"environment", "hdri_intensity", "HDRI intensity", T::number,
                      Env::k_intensity.min, Env::k_intensity.max, Env{}.hdri_intensity, "",
                      "Scales the HDRI's background, lighting and sun",
                      &field_ref<&S::environment, &Env::hdri_intensity>},
    RenderSettingDesc{"environment", "hdri_rotation_deg", "HDRI rotation", T::number,
                      Env::k_rotation_deg.min, Env::k_rotation_deg.max, Env{}.hdri_rotation_deg,
                      "deg", "Turns the HDRI about the vertical axis",
                      &field_ref<&S::environment, &Env::hdri_rotation_deg>},
    RenderSettingDesc{"environment", "hdri_sun", "HDRI sun", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "The HDRI's sun becomes the shadowed directional light",
                      &field_ref<&S::environment, &Env::hdri_sun>},
    // ---- ssr (R21) ----
    RenderSettingDesc{"ssr", "enabled", "enabled", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "Trace screen-space reflections; off leaves the environment only",
                      &field_ref<&S::ssr, &Ssr::enabled>},
    RenderSettingDesc{"ssr", "quality", "quality", T::choice, 0.0F,
                      static_cast<float>(ssr_quality_names.size() - 1), 1.0F, "",
                      "low: 32 steps, 1 reused ray; medium: 64, 4; high: 128, 4",
                      &field_ref<&S::ssr, &Ssr::quality>, ssr_quality_names},
    RenderSettingDesc{"ssr", "max_roughness", "max roughness", T::number,
                      Ssr::k_max_roughness.min, Ssr::k_max_roughness.max, Ssr{}.max_roughness,
                      "", "Rougher surfaces use the environment only",
                      &field_ref<&S::ssr, &Ssr::max_roughness>},
    RenderSettingDesc{"ssr", "thickness", "thickness", T::number, Ssr::k_thickness.min,
                      Ssr::k_thickness.max, Ssr{}.thickness, "m",
                      "How far behind the depth buffer a ray still hits",
                      &field_ref<&S::ssr, &Ssr::thickness>},
    RenderSettingDesc{"ssr", "edge_fade", "edge fade", T::number, Ssr::k_edge_fade.min,
                      Ssr::k_edge_fade.max, Ssr{}.edge_fade, "",
                      "Screen-edge band where reflections fade to the environment",
                      &field_ref<&S::ssr, &Ssr::edge_fade>},
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
    RenderSettingDesc{"clouds", "shadows", "shadows", T::boolean, 0.0F, 1.0F, 1.0F, "",
                      "With weather: clouds shadow the sun on the ground",
                      &field_ref<&S::clouds, &CloudSettings::shadows>},
};

constexpr auto k_fields = concat_fields(k_core_fields, k_post_fields);

// Keys of the pre-R12 single shadow map (one orthographic box), still
// accepted from older scene files and clients.
constexpr std::array k_deprecated = {
    DeprecatedRenderSetting{"shadows", "extent_m",
                            "replaced by max_distance (the cascades follow the camera)"},
    DeprecatedRenderSetting{"shadows", "depth_bias", "replaced by depth_bias_texels"},
    DeprecatedRenderSetting{"shadows", "slope_bias",
                            "replaced by the per-tap receiver-plane bias and normal_bias_texels"},
};

} // namespace

std::span<const RenderSettingGroup> render_setting_groups() { return k_groups; }

SkySource sky_source(const EnvironmentSettings& settings) {
    return settings.source == static_cast<int>(SkySource::hdri) ? SkySource::hdri
                                                                : SkySource::procedural;
}

std::span<const DeprecatedRenderSetting> deprecated_render_settings() { return k_deprecated; }

const DeprecatedRenderSetting* find_deprecated_render_setting(std::string_view group,
                                                             std::string_view name) {
    for (const DeprecatedRenderSetting& d : k_deprecated) {
        if (d.group == group && d.name == name) return &d;
    }
    return nullptr;
}

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
    if (ref.f != nullptr) return *ref.f;
    return 0.0F;
}

std::string render_setting_text(const RenderSettings& settings, const RenderSettingDesc& field) {
    RenderSettings copy = settings;
    const RenderSettingRef ref = field.ref(copy);
    return ref.s != nullptr ? *ref.s : std::string();
}

std::string check_render_setting_path(const RenderSettingDesc& field, std::string_view text) {
    if (field.type != RenderSettingType::path) return "not a path field";
    if (text.empty()) return {};
    if (text.size() > 256) return "path longer than 256 bytes";
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '\\' || c == ':') {
            return "path has a control character, backslash or colon (use a relative path "
                   "with '/')";
        }
    }
    if (text.front() == '/') return "path must be relative to the project root";
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('/', start), text.size());
        if (text.substr(start, end - start) == "..") return "path must not contain '..'";
        start = end + 1;
    }
    const std::string_view ext = field.unit;
    if (!ext.empty() && (text.size() <= ext.size() ||
                         text.substr(text.size() - ext.size()) != ext)) {
        return "expected a " + std::string(ext) + " file";
    }
    return {};
}

bool set_render_setting_text(RenderSettings& settings, const RenderSettingDesc& field,
                             std::string_view text) {
    const RenderSettingRef ref = field.ref(settings);
    if (ref.s == nullptr || !check_render_setting_path(field, text).empty()) return false;
    *ref.s = std::string(text);
    return true;
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
    if (ref.s != nullptr) return;
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
        const glm::vec2 offset = settings.clouds.offset_km;
        settings = RenderSettings{};
        settings.clouds.time_s = time;
        settings.clouds.offset_km = offset;
        return true;
    }
    if (find_render_setting_group(group) == nullptr) return false;
    for (const RenderSettingDesc& d : k_fields) {
        if (d.group != group) continue;
        if (d.type == RenderSettingType::path) {
            (void)set_render_setting_text(settings, d, "");
        } else {
            set_render_setting(settings, d, d.default_value);
        }
    }
    return true;
}

} // namespace engine::renderer
