//! Inputs of the debugging tools (`debug_*`, docs/remote-control.md
//! "Debugging methods"), and the shape of the engine's `debug.log` reply.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

use super::EntityRef;

/// Minimum severity of log lines to return.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DebugLogParams {
    /// Return only lines after this sequence number: pass the `cursor` of the
    /// previous call to read what is new. Omit for the newest lines.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub since_seq: Option<u64>,
    /// Minimum level: trace < debug < info < warn < error.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub level: Option<LogLevel>,
    /// Only this category (exact), e.g. "vulkan", "nav", "remote", "stderr".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub category: Option<String>,
    /// Only lines whose message contains this text (case-sensitive).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub contains: Option<String>,
    /// Most lines to return, 1..1000 (default 100).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub limit: Option<u32>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DebugStepParams {
    /// Frames to advance, 1..10000 (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frames: Option<u32>,
    /// Seconds each step advances the simulation, 0.0001..0.1 (default 1/60).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub dt: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DebugTimeScaleParams {
    /// Simulation speed multiplier: 1 = real time, 0.25 = slow motion; 0.01..10.
    pub scale: f64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DebugWatchParams {
    /// One entity: integer id (preferred) or exact unique name. Give this or `entities`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
    /// Several entities (up to 64). Give this or `entity`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entities: Option<Vec<EntityRef>>,
    /// Only these components, e.g. `["transform", "rigidbody"]`. Omit for all.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub components: Option<Vec<String>>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DebugRaycastParams {
    /// Ray start, world-space [x, y, z] in metres (Y is up).
    pub origin: [f64; 3],
    /// Ray direction [x, y, z]; need not be normalized, must not be zero.
    pub direction: [f64; 3],
    /// Longest distance to test, metres (default 1000).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_distance: Option<f64>,
    /// Also hit trigger (sensor) volumes (default false).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub include_sensors: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DebugNavPathParams {
    /// Start, world-space [x, y, z] (e.g. an agent's `world_position`).
    pub from: [f64; 3],
    /// Destination, world-space [x, y, z] (e.g. the agent's `nav_agent.target`).
    pub to: [f64; 3],
}

/// One line of the engine's `debug.log` reply.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LogLine {
    pub seq: u64,
    /// Seconds since the engine log started.
    pub t: f64,
    pub level: String,
    pub category: String,
    pub message: String,
}

/// The engine's `debug.log` reply.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LogReply {
    pub lines: Vec<LogLine>,
    pub cursor: u64,
    pub more: bool,
    pub missed: u64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn log_params_serialize_only_what_is_given() {
        let p: DebugLogParams =
            serde_json::from_value(json!({"level": "warn", "since_seq": 12})).unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"since_seq": 12, "level": "warn"})
        );
        assert!(serde_json::from_value::<DebugLogParams>(json!({"level": "loud"})).is_err());
        assert!(serde_json::from_value::<DebugLogParams>(json!({"since": 1})).is_err());
    }

    #[test]
    fn watch_params_accept_ids_and_names() {
        let p: DebugWatchParams =
            serde_json::from_value(json!({"entities": [3, "Crate"], "components": ["transform"]}))
                .unwrap();
        assert_eq!(
            p.entities,
            Some(vec![EntityRef::Id(3), EntityRef::Name("Crate".into())])
        );
        assert!(p.entity.is_none());
    }

    #[test]
    fn raycast_params_require_origin_and_direction() {
        assert!(
            serde_json::from_value::<DebugRaycastParams>(json!({"origin": [0, 1, 0]})).is_err()
        );
        let p: DebugRaycastParams =
            serde_json::from_value(json!({"origin": [0, 1, 0], "direction": [0, -1, 0]})).unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"origin": [0.0, 1.0, 0.0], "direction": [0.0, -1.0, 0.0]})
        );
    }
}
