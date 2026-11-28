//! Inputs of the input-action tools (`input_*`, ROADMAP G5) and the camera
//! rig tools (`camera_rig_*`, ROADMAP G15.1); docs/remote-control.md has the
//! engine methods behind them.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use super::{Components, EntityRef};

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct InputActionsParams {
    /// Only this action (exact name). Omit to list every action.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub action: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct InputInjectParams {
    /// Action name, e.g. "jump" or "camera.pan" (`input_actions` lists them).
    pub action: String,
    /// What the action reads: true / false for a button, a number for an
    /// axis1d, [x, y] for an axis2d (y is forward / up).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub value: Option<InjectValue>,
    /// For how many frames (1..36000, default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frames: Option<u32>,
    /// true: stop an injection early (give no value).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clear: Option<bool>,
}

/// An injected action value.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum InjectValue {
    /// A button: pressed or released.
    Button(bool),
    /// An axis1d value.
    Axis(f64),
    /// An axis2d value [x, y].
    Axis2d([f64; 2]),
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct InputBindParams {
    /// Action name.
    pub action: String,
    /// The new bindings ("key:w", "key:ctrl+z", "mouse:left", "gamepad:south",
    /// or binding objects), replacing the old ones; null restores the action
    /// map's own bindings.
    pub bindings: Option<Vec<Value>>,
    /// Write the user's rebinding file (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub save: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct InputContextParams {
    /// Context to push on top of the stack.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub push: Option<String>,
    /// Context to remove from the stack.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pop: Option<String>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CameraRigGetParams {
    /// Entity carrying the `camera_rig` (id or unique name). Omit for the
    /// rig on the active camera (or the only rig).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CameraRigSetParams {
    /// Entity carrying the `camera_rig`. Omit for the active camera's rig.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
    /// Ground point [x, y, z] the camera orbits, metres.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub focus: Option<[f64; 3]>,
    /// Heading in degrees: 0 looks along +X, 90 along +Z.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub yaw_deg: Option<f64>,
    /// Pitch in degrees (negative looks down); used when `pitch_follows_zoom`
    /// is false.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pitch_deg: Option<f64>,
    /// Metres from the focus (clamped to `min_distance`..`max_distance`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub distance: Option<f64>,
    /// true: jump there now (no smoothing) — for deterministic screenshots.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub immediate: Option<bool>,
    /// Any other `camera_rig` fields (limits, half-lives, bounds, ...).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub more: Option<Components>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CameraRigFrameParams {
    /// Entity carrying the `camera_rig`. Omit for the active camera's rig.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
    /// Entity (and its children) to fit in view. Omit for the whole scene.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<EntityRef>,
    /// Fraction of the view kept free at each side (0..0.9, default 0.1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub margin: Option<f64>,
    /// true: jump there now (no smoothing).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub immediate: Option<bool>,
}
