//! Typed inputs of the world-building tools added for ROADMAP A9: camera clip
//! planes, terrain status, placement, explicit-camera screenshots and the
//! Poly Haven asset tools.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize, Serializer};

use super::EntityRef;

/// A clip plane: a distance in metres, or "auto" for the plane the engine
/// derives from the scene (drawables and terrain).
#[derive(Debug, Clone, Copy, PartialEq, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum ClipPlane {
    /// Distance from the camera in metres.
    Metres(f64),
    /// Return to the plane derived from the scene.
    Auto(AutoKeyword),
}

/// The keyword "auto".
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum AutoKeyword {
    /// Derived from the scene.
    Auto,
}

impl Serialize for ClipPlane {
    /// The engine reads `null` as "derived from the scene".
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Metres(m) => s.serialize_f64(*m),
            Self::Auto(_) => s.serialize_none(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct PlaceOnGroundParams {
    /// The entity to place (id or unique name); its children move with it.
    pub entity: EntityRef,
    /// Move it to this world [x, z] (metres) first.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub at: Option<[f64; 2]>,
    /// Tilt its up axis to the surface normal (default false).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub align_to_normal: Option<bool>,
    /// Largest tilt in degrees when aligning (0..90, default 30).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tilt_deg: Option<f64>,
    /// Metres added along world up after resting it (negative sinks it).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub offset: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ShotCamera {
    /// Camera position [x, y, z] in metres.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub position: Option<[f64; 3]>,
    /// World point [x, y, z] to aim at (instead of yaw/pitch).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub look_at: Option<[f64; 3]>,
    /// Heading in degrees: 0 looks along +X, 90 along +Z.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub yaw_deg: Option<f64>,
    /// Elevation in degrees: positive looks up (-89..89).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pitch_deg: Option<f64>,
    /// Entity (id or unique name) to fit in view, like `camera_frame`
    /// (instead of position / `look_at`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frame: Option<EntityRef>,
    /// Vertical field of view in degrees.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fov_deg: Option<f64>,
    /// Near clip plane in metres, or "auto".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub near: Option<ClipPlane>,
    /// Far clip plane in metres, or "auto".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub far: Option<ClipPlane>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn clip_planes_map_auto_to_null() {
        let near: ClipPlane = serde_json::from_value(json!(0.5)).unwrap();
        assert_eq!(serde_json::to_value(near).unwrap(), json!(0.5));
        let auto: ClipPlane = serde_json::from_value(json!("auto")).unwrap();
        assert_eq!(serde_json::to_value(auto).unwrap(), json!(null));
        assert!(serde_json::from_value::<ClipPlane>(json!("far")).is_err());
    }
}
