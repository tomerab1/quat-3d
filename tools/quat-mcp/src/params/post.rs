//! Inputs of the cinematic post-stack groups of `render_settings_set`
//! (ROADMAP R20: depth of field, motion blur, tonemapping, colour grading,
//! lens and film effects). Ranges here are summaries; the engine's
//! `render_settings_schema` is authoritative and the engine range-checks
//! every value. The lens itself (aperture, shutter, ISO, focal length,
//! focus distance) is on the `camera` component.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DofParams {
    /// Depth of field on/off (default off); the active camera's lens sets the blur.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Largest blur (circle of confusion) radius in pixels, 4..64 (default 24).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_radius_px: Option<f64>,
    /// Gather rings per field, integer 2..6 (default 4): smoother bokeh, slower.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rings: Option<u32>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct MotionBlurParams {
    /// Motion blur on/off (default on); length = motion while the camera's shutter is open.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Longest blur radius in pixels (also the tile size), 8..64 (default 32).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_radius_px: Option<f64>,
    /// Reconstruction samples per pixel, integer 4..32 (default 12).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub samples: Option<u32>,
    /// Multiplier on the camera's shutter fraction, 0..2 (default 1 = physical).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub scale: Option<f64>,
}

/// HDR to display operator.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum TonemapOperator {
    /// Filmic ACES fit: contrasty and saturated (default).
    Aces,
    /// Blender 4 style `AgX`: natural highlight roll-off toward white.
    Agx,
    /// Khronos PBR Neutral: keeps base colours, for product shots.
    PbrNeutral,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TonemapParams {
    /// `aces` (default), `agx` or `pbr_neutral`.
    #[serde(default, rename = "operator", skip_serializing_if = "Option::is_none")]
    pub op: Option<TonemapOperator>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct GradingParams {
    /// White balance: light colour temperature rendered as white, K 1500..15000 (default 6500; lower = cooler image).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature_k: Option<f64>,
    /// White balance tint, -1 (green) .. 1 (magenta) (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tint: Option<f64>,
    /// Contrast around mid-grey, 0..2 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub contrast: Option<f64>,
    /// Colourfulness, 0 (monochrome) .. 2 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub saturation: Option<f64>,
    /// Shadow offset, red, -0.5..0.5 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lift_r: Option<f64>,
    /// Shadow offset, green, -0.5..0.5 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lift_g: Option<f64>,
    /// Shadow offset, blue, -0.5..0.5 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lift_b: Option<f64>,
    /// Mid-tone gamma, red, 0.2..5 (default 1; above 1 brightens).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gamma_r: Option<f64>,
    /// Mid-tone gamma, green, 0.2..5 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gamma_g: Option<f64>,
    /// Mid-tone gamma, blue, 0.2..5 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gamma_b: Option<f64>,
    /// Highlight gain, red, 0..4 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gain_r: Option<f64>,
    /// Highlight gain, green, 0..4 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gain_g: Option<f64>,
    /// Highlight gain, blue, 0..4 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gain_b: Option<f64>,
    /// Look LUT: project-relative `.cube` file ("" = none), applied after tonemapping.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lut: Option<String>,
    /// Blend toward the look LUT's output, 0..1 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lut_contribution: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct FilmParams {
    /// Lens cos^4 light falloff toward the corners, 0..1 (default 0; 1 = physical).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub vignette_natural: Option<f64>,
    /// Artistic corner darkening, 0..1 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub vignette_intensity: Option<f64>,
    /// Width of the artistic vignette's falloff, 0.05..1 (default 0.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub vignette_smoothness: Option<f64>,
    /// Film grain strength, 0..1 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub grain_intensity: Option<f64>,
    /// Grain size in pixels, 0.5..4 (default 1.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub grain_size_px: Option<f64>,
    /// Lateral colour fringing toward the corners, 0..1 (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chromatic_aberration: Option<f64>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn operator_and_lut_round_trip_by_name() {
        let t: TonemapParams = serde_json::from_value(json!({"operator": "pbr_neutral"})).unwrap();
        assert_eq!(t.op, Some(TonemapOperator::PbrNeutral));
        assert_eq!(
            serde_json::to_value(&t).unwrap(),
            json!({"operator": "pbr_neutral"})
        );
        let g: GradingParams =
            serde_json::from_value(json!({"lut": "assets/luts/warm.cube", "gain_b": 1.1})).unwrap();
        assert_eq!(g.lut.as_deref(), Some("assets/luts/warm.cube"));
        assert!(serde_json::from_value::<FilmParams>(json!({"grain": 1})).is_err());
    }
}
