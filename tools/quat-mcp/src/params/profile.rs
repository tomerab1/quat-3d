//! Inputs of the profiling tools (`profile_*`, docs/remote-control.md
//! "Profiling"), and the parts of the engine's `profile.capture` /
//! `profile.result` replies the capture tool reads while it polls.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// Order of the zone table.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum ZoneSort {
    /// The larger of CPU self and GPU average: what the zone itself costs (default).
    Cost,
    /// CPU self average: the zone's own time, nested zones excluded.
    #[serde(rename = "self")]
    SelfAvg,
    #[serde(rename = "self_p95")]
    SelfP95,
    #[serde(rename = "self_max")]
    SelfMax,
    /// CPU average, inclusive of nested zones.
    Cpu,
    CpuP95,
    CpuMax,
    /// GPU average.
    Gpu,
    GpuP95,
    GpuMax,
    /// Alphabetical.
    Name,
}

/// Which zones to list.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum ZoneKind {
    All,
    /// Render-graph passes (CPU recording time and GPU time).
    Pass,
    /// Scene systems (physics, animation, navigation, scripts, audio, ...).
    System,
    /// Other instrumented scopes (frame loop, uploads, editor UI).
    Scope,
    /// Time blocked on the GPU or the display (fence wait, acquire, present).
    Wait,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ProfileFrameParams {
    /// Zone order (default `cost`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sort: Option<ZoneSort>,
    /// Only zones of this kind (default `all`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kind: Option<ZoneKind>,
    /// Most zones to list, 1..200 (default 30).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub limit: Option<u32>,
}

/// Longest `max_wait_s` a `profile_capture` call accepts.
pub const MAX_CAPTURE_WAIT_S: u32 = 120;
/// `max_wait_s` when not given.
pub const DEFAULT_CAPTURE_WAIT_S: u32 = 30;

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ProfileCaptureParams {
    /// Keep waiting for an earlier capture (the id a previous call returned)
    /// instead of starting a new one. Do not combine with the settings below.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<u32>,
    /// Frames to record, 1..1000 (default 120, about 2 s at 60 fps).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frames: Option<u32>,
    /// Frame-time budget in milliseconds, 0.1..1000 (default 16.667 = 60 fps):
    /// frames over it are reported as spikes.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub budget_ms: Option<f64>,
    /// Zones per ranking, 1..50 (default 10).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub top: Option<u32>,
    /// Longest time this call waits for the capture to finish, in seconds,
    /// 1..120 (default 30). Longer captures can be awaited by calling again
    /// with `id`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_wait_s: Option<u32>,
}

/// What `profile.capture` takes: the capture settings of
/// [`ProfileCaptureParams`] (not `id` or `max_wait_s`, which the tool uses).
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CaptureSettings {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub frames: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub budget_ms: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub top: Option<u32>,
}

impl ProfileCaptureParams {
    /// The settings to send with `profile.capture`.
    #[must_use]
    pub fn settings(&self) -> CaptureSettings {
        CaptureSettings {
            frames: self.frames,
            budget_ms: self.budget_ms,
            top: self.top,
        }
    }

    /// Whether any capture setting was given (they conflict with `id`).
    #[must_use]
    pub fn has_settings(&self) -> bool {
        self.frames.is_some() || self.budget_ms.is_some() || self.top.is_some()
    }
}

/// The engine's `profile.capture` reply (the fields the tool needs).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct CaptureStarted {
    pub id: u32,
    pub frames: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum CaptureState {
    Recording,
    Done,
}

/// The parts of a `profile.result` reply that drive polling. A finished
/// capture's summary is passed to the model as the engine sent it.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct CaptureStatus {
    pub id: u32,
    pub status: CaptureState,
    #[serde(default)]
    pub frames: u32,
    #[serde(default)]
    pub frames_done: u32,
    #[serde(default)]
    pub waiting_for_gpu: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn frame_params_use_the_engine_spellings() {
        let p: ProfileFrameParams =
            serde_json::from_value(json!({"sort": "gpu_p95", "kind": "pass", "limit": 5})).unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"sort": "gpu_p95", "kind": "pass", "limit": 5})
        );
        assert_eq!(
            serde_json::to_value(ProfileFrameParams::default()).unwrap(),
            json!({})
        );
        assert!(serde_json::from_value::<ProfileFrameParams>(json!({"sort": "fast"})).is_err());
        for sort in ["self", "self_p95", "self_max"] {
            let p: ProfileFrameParams = serde_json::from_value(json!({"sort": sort})).unwrap();
            assert_eq!(serde_json::to_value(&p).unwrap(), json!({"sort": sort}));
        }
        assert!(serde_json::from_value::<ProfileFrameParams>(json!({"window": 10})).is_err());
    }

    #[test]
    fn capture_settings_leave_out_the_tool_only_fields() {
        let p: ProfileCaptureParams =
            serde_json::from_value(json!({"frames": 300, "budget_ms": 8.33, "max_wait_s": 10}))
                .unwrap();
        assert!(p.has_settings());
        assert_eq!(
            serde_json::to_value(p.settings()).unwrap(),
            json!({"frames": 300, "budget_ms": 8.33})
        );
        let poll: ProfileCaptureParams = serde_json::from_value(json!({"id": 4})).unwrap();
        assert!(!poll.has_settings());
        assert_eq!(serde_json::to_value(poll.settings()).unwrap(), json!({}));
    }

    #[test]
    fn capture_replies_parse() {
        let started: CaptureStarted = serde_json::from_value(
            json!({"id": 3, "status": "recording", "frames": 120, "budget_ms": 16.667}),
        )
        .unwrap();
        assert_eq!(started, CaptureStarted { id: 3, frames: 120 });

        let recording: CaptureStatus = serde_json::from_value(json!({
            "id": 3, "status": "recording", "frames": 120, "frames_done": 40,
            "waiting_for_gpu": false
        }))
        .unwrap();
        assert_eq!(recording.status, CaptureState::Recording);
        assert_eq!(recording.frames_done, 40);

        let done: CaptureStatus = serde_json::from_value(
            json!({"id": 3, "status": "done", "frames": 120, "top_by_total": []}),
        )
        .unwrap();
        assert_eq!(done.status, CaptureState::Done);
        assert!(
            serde_json::from_value::<CaptureStatus>(json!({"id": 3, "status": "lost"})).is_err()
        );
    }
}
