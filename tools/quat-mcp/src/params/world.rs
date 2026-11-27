//! Typed inputs of the world-building tools added for ROADMAP A9: camera clip
//! planes, terrain status, placement, explicit-camera screenshots and the
//! Poly Haven asset tools.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize, Serializer};

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
