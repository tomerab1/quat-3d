//! Inputs of the look-tuning tools (`render_settings_*`, `material_*`;
//! docs/remote-control.md "Look tuning"). Ranges here are summaries; the
//! engine's `render_settings_schema` is authoritative and the engine
//! range-checks every value.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use super::{CloudParams, Color, EntityRef};

/// A render-settings group.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum SettingsGroup {
    Exposure,
    Bloom,
    Taa,
    Shadows,
    Ibl,
    Clouds,
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

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ShadowParams {
    /// Sun shadows on/off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Half-size in metres of the shadowed box around the world origin, 2..500 (default 14).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub extent_m: Option<f64>,
    /// Constant depth bias against acne, 0..0.05 (default 0.0008).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub depth_bias: Option<f64>,
    /// Extra bias on surfaces facing away from the sun, 0..0.05 (default 0.0025).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub slope_bias: Option<f64>,
    /// Edge softness: filter radius in texels, integer 0..3 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pcf_radius: Option<u32>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct IblParams {
    /// Scale of the sky's ambient light and reflections, 0..4 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub intensity: Option<f64>,
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
    /// Sun shadow map.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub shadows: Option<ShadowParams>,
    /// Image-based (sky) ambient light and reflections.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ibl: Option<IblParams>,
    /// Volumetric clouds (same fields as `environment_set`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clouds: Option<CloudParams>,
}

impl RenderSettingsSetParams {
    /// True when no group is given (nothing to change).
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.exposure.is_none()
            && self.bloom.is_none()
            && self.taa.is_none()
            && self.shadows.is_none()
            && self.ibl.is_none()
            && self.clouds.is_none()
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
            json!({"exposure": {"auto": false, "compensation_ev": 1.5}, "shadows": {"pcf_radius": 2}}),
        )
        .unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"exposure": {"auto": false, "compensation_ev": 1.5}, "shadows": {"pcf_radius": 2}})
        );
        assert!(!p.is_empty());
        assert!(RenderSettingsSetParams::default().is_empty());
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
