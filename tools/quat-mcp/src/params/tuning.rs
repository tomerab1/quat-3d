//! Inputs of the look-tuning tools (`render_settings_*`, `material_*`;
//! docs/remote-control.md "Look tuning"). Ranges here are summaries; the
//! engine's `render_settings_schema` is authoritative and the engine
//! range-checks every value.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use super::{
    CloudParams, Color, DofParams, EntityRef, FilmParams, GradingParams, MotionBlurParams,
    SkySource, TonemapParams,
};

/// A render-settings group.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum SettingsGroup {
    Exposure,
    Bloom,
    Taa,
    Upscaler,
    Shadows,
    Ao,
    #[serde(rename = "contact_shadows")]
    ContactShadows,
    Ibl,
    Environment,
    Ssr,
    Clouds,
    Dof,
    #[serde(rename = "motion_blur")]
    MotionBlur,
    Tonemap,
    Grading,
    Film,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SettingsGroupParams {
    /// Only this group. Omit for every group.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub group: Option<SettingsGroup>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ExposureParams {
    /// Auto exposure (adapts to scene brightness); false = fixed exposure `2^compensation_ev`.
    #[serde(default, rename = "auto", skip_serializing_if = "Option::is_none")]
    pub auto_exposure: Option<bool>,
    /// Exposure bias in stops (EV), -10..10; +1 doubles the brightness.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub compensation_ev: Option<f64>,
    /// Mid-grey the average scene luminance maps to, 0.01..1 (default 0.18).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub key: Option<f64>,
    /// Lowest auto exposure multiplier, 0.001..64 (default 0.05).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min_exposure: Option<f64>,
    /// Highest auto exposure multiplier, 0.001..64 (default 16).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_exposure: Option<f64>,
    /// Eye adaptation time constant in seconds, 0.05..10 (default 1.1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub adaptation_s: Option<f64>,
    /// Expose from the active camera's aperture, shutter and ISO (EV100); overrides `auto`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub physical: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BloomParams {
    /// Run the bloom pass.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// HDR luminance above which highlights glow, 0..8 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub threshold: Option<f64>,
    /// Softness of the threshold, 0..1 (default 0.6).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub knee: Option<f64>,
    /// Glow strength, 0..1 (default 0.06).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub intensity: Option<f64>,
    /// Glow spread, 0.2..2 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radius: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TaaParams {
    /// Temporal anti-aliasing on/off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// History weight, 0..0.98 (default 0.9): higher is smoother but ghosts more.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub feedback: Option<f64>,
}

/// Render-resolution mode of the temporal upscaler.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum UpscalerMode {
    /// `quality` on high-density displays (display scale >= 1.5), else `native_aa`.
    Auto,
    /// Full resolution, anti-aliasing only.
    NativeAa,
    /// Render scale 1/1.5 per axis.
    Quality,
    /// Render scale 1/1.7 per axis.
    Balanced,
    /// Render scale 1/2 per axis.
    Performance,
}

/// Upscaler implementation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum UpscalerBackend {
    /// The engine's temporal upsampler.
    Taau,
    /// AMD FSR 2: not built yet, the engine falls back to `taau`.
    Fsr2,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct UpscalerParams {
    /// Render resolution (needs `taa.enabled`): `auto` (default), `native_aa`, `quality`, `balanced` or `performance`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<UpscalerMode>,
    /// Render scale per axis overriding the mode when above 0, 0..1 (min effective 0.25; default 0 = off).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub render_scale: Option<f64>,
    /// Sharpening of the reconstructed frame, 0..1 (default 0.25).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sharpness: Option<f64>,
    /// Implementation: `taau` (default) or `fsr2` (not built yet).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub backend: Option<UpscalerBackend>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ShadowParams {
    /// Cascaded sun shadows on/off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Metres from the camera where shadows end, 5..5000 (default 150).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_distance: Option<f64>,
    /// Number of cascades, integer 1..4 (default 4).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cascades: Option<u32>,
    /// Cascade split spacing, 0 (linear) .. 1 (logarithmic) (default 0.8).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub split_lambda: Option<f64>,
    /// Fit the cascades to the frame's depth range on the GPU (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fit_to_depth: Option<bool>,
    /// Fraction of each cascade cross-faded into the next, 0..0.5 (default 0.1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub blend_band: Option<f64>,
    /// Edge softness: filter radius in texels, 0..8 (default 1.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pcf_radius: Option<f64>,
    /// Sun angular diameter in degrees for contact-hardening penumbrae, 0..10 (default 0.53; 0 = fixed filter).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sun_size_deg: Option<f64>,
    /// Receiver offset along the normal against acne, texels 0..8 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub normal_bias_texels: Option<f64>,
    /// Receiver offset toward the sun against acne, texels 0..8 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub depth_bias_texels: Option<f64>,
    /// How far toward the sun casters are captured, metres 1..10000 (default 200).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub caster_distance: Option<f64>,
    /// Tint each pixel by its cascade (debug view).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub debug_cascades: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct AoParams {
    /// Ground-truth ambient occlusion (GTAO) on/off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// World-space reach of the occlusion search, metres 0.05..10 (default 0.75).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub radius: Option<f64>,
    /// Darkening strength (visibility exponent), 0.5..5 (default 2.2).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub strength: Option<f64>,
    /// Fraction of the radius over which occluders fade out, 0..1 (default 0.615).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub falloff_range: Option<f64>,
    /// Above 1 packs samples near the pixel, 1..4 (default 2).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sample_distribution_power: Option<f64>,
    /// Lets light pass behind thin objects, 0..0.7 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thin_occluder_compensation: Option<f64>,
    /// Hi-Z mip bias for distant samples, 0..8 (default 3.3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub depth_mip_offset: Option<f64>,
    /// Search directions per pixel, integer 1..9 (default 3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub slices: Option<u32>,
    /// Depth samples per direction and side, integer 1..8 (default 3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub steps_per_slice: Option<u32>,
    /// Edge-aware blur passes, integer 0..3 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub denoise_passes: Option<u32>,
    /// Denoiser centre weight: higher keeps more detail, 0.1..10 (default 1.2).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub denoise_blur_beta: Option<f64>,
    /// Light the ambient term along the least-occluded direction (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bent_normals: Option<bool>,
    /// Occlude environment reflections in crevices (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub specular_occlusion: Option<bool>,
    /// Brighten occlusion on light surfaces (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub multi_bounce: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ContactShadowParams {
    /// Screen-space contact shadows on/off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Ray length toward the sun, metres 0.01..4 (default 0.35).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub length: Option<f64>,
    /// Depth samples along the ray, integer 2..64 (default 12).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub steps: Option<u32>,
    /// How far behind the depth buffer a hit still counts, metres 0.01..4 (default 0.2).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thickness: Option<f64>,
    /// 0 = no effect, 1 = full shadow on a hit (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub intensity: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct IblParams {
    /// Scale of the sky's ambient light and reflections, 0..4 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub intensity: Option<f64>,
    /// Multiple-scattering energy compensation (rough metals keep their brightness).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub multiscatter: Option<bool>,
    /// Fade normal-mapped reflections that point below the surface.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub horizon_occlusion: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct EnvironmentSettingsParams {
    /// Sky source: procedural or hdri (load the file with `environment_set`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<SkySource>,
    /// HDRI brightness, 0..100 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hdri_intensity: Option<f64>,
    /// HDRI rotation about the vertical axis, degrees, -360..360 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hdri_rotation_deg: Option<f64>,
    /// The HDRI's sun drives the shadowed sun light (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hdri_sun: Option<bool>,
}

/// Screen-space reflection quality.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum SsrQuality {
    Low,
    Medium,
    High,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SsrParams {
    /// Trace screen-space reflections (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// low (32 steps, 1 reused ray), medium (64, 4; default), high (128, 4).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub quality: Option<SsrQuality>,
    /// Surfaces rougher than this use the environment only, 0..1 (default 0.6).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_roughness: Option<f64>,
    /// Metres behind the depth buffer a ray still counts as a hit, 0.01..5 (default 0.25).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thickness: Option<f64>,
    /// Screen-edge fade band, 0..0.5 of the screen height (default 0.1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub edge_fade: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RenderSettingsSetParams {
    /// Exposure: auto / manual, compensation in EV, auto-exposure limits.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub exposure: Option<ExposureParams>,
    /// Bloom (glow around bright highlights).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bloom: Option<BloomParams>,
    /// Temporal anti-aliasing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub taa: Option<TaaParams>,
    /// Render resolution and temporal upscaling.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub upscaler: Option<UpscalerParams>,
    /// Cascaded sun shadows.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub shadows: Option<ShadowParams>,
    /// Ambient occlusion (GTAO).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ao: Option<AoParams>,
    /// Screen-space contact shadows.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub contact_shadows: Option<ContactShadowParams>,
    /// Image-based (sky) ambient light and reflections.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ibl: Option<IblParams>,
    /// Sky source and HDRI look (the HDRI file is set with `environment_set`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub environment: Option<EnvironmentSettingsParams>,
    /// Screen-space reflections.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ssr: Option<SsrParams>,
    /// Volumetric clouds (same fields as `environment_set`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clouds: Option<CloudParams>,
    /// Depth of field from the active camera's lens.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dof: Option<DofParams>,
    /// Motion blur over the active camera's shutter.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub motion_blur: Option<MotionBlurParams>,
    /// Tonemap operator.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tonemap: Option<TonemapParams>,
    /// Colour grading: white balance, contrast, lift / gamma / gain, look LUT.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub grading: Option<GradingParams>,
    /// Lens and film: vignette, grain, chromatic aberration.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub film: Option<FilmParams>,
}

impl RenderSettingsSetParams {
    /// True when no group is given (nothing to change).
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.exposure.is_none()
            && self.bloom.is_none()
            && self.taa.is_none()
            && self.upscaler.is_none()
            && self.shadows.is_none()
            && self.ao.is_none()
            && self.contact_shadows.is_none()
            && self.ibl.is_none()
            && self.environment.is_none()
            && self.ssr.is_none()
            && self.clouds.is_none()
            && self.dof.is_none()
            && self.motion_blur.is_none()
            && self.tonemap.is_none()
            && self.grading.is_none()
            && self.film.is_none()
    }
}

/// A material factor `material_set` can revert.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum MaterialField {
    BaseColor,
    Emissive,
    Metallic,
    Roughness,
    OcclusionStrength,
    NormalScale,
    AlphaCutoff,
    Transmission,
    Ior,
    AttenuationColor,
    AttenuationDistance,
    Thickness,
    Clearcoat,
    ClearcoatRoughness,
}

/// Material factors; omitted fields keep their values.
#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct MaterialValues {
    /// Base colour: "#rrggbb" / "#rrggbbaa" (sRGB) or linear [r, g, b(, a)] in 0..1; alpha < 1 on a primitive makes it translucent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub base_color: Option<Color>,
    /// Emitted light: linear [r, g, b] >= 0 (HDR, may exceed 1) or "#rrggbb".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub emissive: Option<Color>,
    /// 0 = dielectric (plastic, wood, stone), 1 = metal.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub metallic: Option<f64>,
    /// 0 = mirror-smooth, 1 = fully rough, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub roughness: Option<f64>,
    /// Strength of the occlusion texture, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub occlusion_strength: Option<f64>,
    /// Strength of the normal map, 0..4.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub normal_scale: Option<f64>,
    /// Alpha-mask threshold, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub alpha_cutoff: Option<f64>,
    /// Refractive glass, 0..1; > 0 renders the object as glass.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub transmission: Option<f64>,
    /// Index of refraction, 1..3 (glass 1.5, water 1.33).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ior: Option<f64>,
    /// Tint light takes on after `attenuation_distance` inside the medium: linear [r, g, b] 0..1 or "#rrggbb".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub attenuation_color: Option<Color>,
    /// Distance for the attenuation colour, 0..10000 (0 = clear medium).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub attenuation_distance: Option<f64>,
    /// Volume thickness in mesh units, 0..10000 (refraction and absorption).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thickness: Option<f64>,
    /// Clear lacquer layer strength, 0..1 (car paint, varnish).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clearcoat: Option<f64>,
    /// Roughness of the lacquer layer, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clearcoat_roughness: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct MaterialGetParams {
    /// The entity (id or unique name); it must be a primitive or a mesh.
    pub entity: EntityRef,
}

#[derive(Debug, Clone, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct MaterialSetParams {
    /// The entity (id or unique name); it must be a primitive or a mesh.
    pub entity: EntityRef,
    /// Factors to set on this entity.
    #[serde(default)]
    pub material: MaterialValues,
    /// Factors to revert to the material's own value (drops this entity's override).
    #[serde(default)]
    pub revert: Vec<MaterialField>,
}

impl MaterialSetParams {
    /// The engine's `material.set` params: the set values plus `null` for
    /// every reverted field. `None` when there is nothing to change.
    #[must_use]
    pub fn to_engine(&self) -> Option<Value> {
        let mut fields = match serde_json::to_value(&self.material) {
            Ok(Value::Object(m)) => m,
            _ => Map::new(),
        };
        for f in &self.revert {
            if let Ok(Value::String(name)) = serde_json::to_value(f) {
                fields.insert(name, Value::Null);
            }
        }
        if fields.is_empty() {
            return None;
        }
        let entity = serde_json::to_value(&self.entity).unwrap_or(Value::Null);
        Some(serde_json::json!({"entity": entity, "material": fields}))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn settings_serialize_only_given_fields() {
        let p: RenderSettingsSetParams = serde_json::from_value(
            json!({"exposure": {"auto": false, "compensation_ev": 1.5}, "shadows": {"pcf_radius": 2.5},
                   "contact_shadows": {"steps": 16}}),
        )
        .unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"exposure": {"auto": false, "compensation_ev": 1.5}, "shadows": {"pcf_radius": 2.5},
                   "contact_shadows": {"steps": 16}})
        );
        let group: SettingsGroup = serde_json::from_value(json!("contact_shadows")).unwrap();
        assert_eq!(group, SettingsGroup::ContactShadows);
        assert!(!p.is_empty());
        assert!(RenderSettingsSetParams::default().is_empty());
    }

    #[test]
    fn reflection_settings_round_trip() {
        let request = json!({
            "ibl": {"multiscatter": false},
            "environment": {"source": "hdri", "hdri_rotation_deg": 45.0},
            "ssr": {"quality": "high", "max_roughness": 0.7}
        });
        let p: RenderSettingsSetParams = serde_json::from_value(request.clone()).unwrap();
        assert!(!p.is_empty());
        assert_eq!(serde_json::to_value(&p).unwrap(), request);
        let group: SettingsGroup = serde_json::from_value(json!("ssr")).unwrap();
        assert_eq!(group, SettingsGroup::Ssr);
        assert!(serde_json::from_value::<SsrParams>(json!({"quality": "ultra"})).is_err());
    }

    #[test]
    fn unknown_settings_are_rejected() {
        let err =
            serde_json::from_value::<RenderSettingsSetParams>(json!({"fog": {}})).unwrap_err();
        assert!(err.to_string().contains("fog"));
        let err = serde_json::from_value::<RenderSettingsSetParams>(json!({"bloom": {"glow": 1}}))
            .unwrap_err();
        assert!(err.to_string().contains("glow"));
        let err =
            serde_json::from_value::<SettingsGroupParams>(json!({"group": "sky"})).unwrap_err();
        assert!(err.to_string().contains("sky"));
    }

    #[test]
    fn material_set_turns_reverts_into_nulls() {
        let p: MaterialSetParams = serde_json::from_value(json!({
            "entity": "car",
            "material": {"roughness": 0.2, "base_color": "#ff0000"},
            "revert": ["clearcoat", "attenuation_color"]
        }))
        .unwrap();
        assert_eq!(
            p.to_engine(),
            Some(json!({"entity": "car", "material": {
                "roughness": 0.2, "base_color": "#ff0000",
                "clearcoat": null, "attenuation_color": null}}))
        );
        let empty: MaterialSetParams = serde_json::from_value(json!({"entity": 3})).unwrap();
        assert_eq!(empty.to_engine(), None);
    }
}
