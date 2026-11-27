//! Profiling tools (ROADMAP R4): the rolling frame profile, frame captures
//! and per-frame counters. Descriptions live in `content/tools/<tool>.md`;
//! the engine methods are documented in docs/remote-control.md
//! ("Profiling").

use std::time::Duration;

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde_json::{Value, json};
use tokio::time::{Instant, sleep};

use super::QuatServer;
use super::results::{error_result, invalid_input, json_result, to_params};
use crate::params::{
    CaptureStarted, CaptureState, CaptureStatus, DEFAULT_CAPTURE_WAIT_S, MAX_CAPTURE_WAIT_S,
    ProfileCaptureParams, ProfileFrameParams,
};

/// How often `profile_capture` asks the engine whether a capture finished.
const POLL_INTERVAL: Duration = Duration::from_millis(250);

/// What a capture poll ended with.
#[derive(Debug, PartialEq)]
enum Polled {
    /// The finished summary, as the engine sent it.
    Done(Value),
    /// Still recording when the wait ran out.
    Pending(CaptureStatus),
}

/// The text returned when the wait runs out before the capture finishes.
fn pending_text(status: &CaptureStatus) -> String {
    let stage = if status.waiting_for_gpu {
        "waiting for the last GPU timings".to_owned()
    } else {
        format!("{}/{} frames", status.frames_done, status.frames)
    };
    let next = json!({"id": status.id});
    format!(
        "Capture {} is still recording ({stage}). Call profile_capture with {next} to keep \
         waiting.",
        status.id
    )
}

impl QuatServer {
    /// Polls `profile.result` for capture `id` until it is done or `wait`
    /// has passed.
    async fn poll_capture(&self, id: u32, wait: Duration) -> Result<Polled, CallToolResult> {
        let deadline = Instant::now() + wait;
        loop {
            let reply = self
                .engine
                .call("profile.result", Some(json!({"id": id})))
                .await
                .map_err(|e| error_result(&e))?;
            // An unexpected shape (a newer engine) is passed through untouched.
            let Ok(status) = serde_json::from_value::<CaptureStatus>(reply.clone()) else {
                return Ok(Polled::Done(reply));
            };
            if status.status == CaptureState::Done {
                return Ok(Polled::Done(reply));
            }
            if Instant::now() + POLL_INTERVAL > deadline {
                return Ok(Polled::Pending(status));
            }
            sleep(POLL_INTERVAL).await;
        }
    }
}

#[tool_router(router = profile_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/profile_frame.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn profile_frame(&self, params: Parameters<ProfileFrameParams>) -> CallToolResult {
        self.run("profile.frame", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/profile_capture.md")]
    #[tool]
    async fn profile_capture(&self, params: Parameters<ProfileCaptureParams>) -> CallToolResult {
        let p = params.0;
        if p.id.is_some() && p.has_settings() {
            return invalid_input(
                "profile_capture: give `id` to keep waiting for an earlier capture, or capture \
                 settings (`frames`, `budget_ms`, `top`) to start a new one, not both.",
            );
        }
        let wait_s = p.max_wait_s.unwrap_or(DEFAULT_CAPTURE_WAIT_S);
        if !(1..=MAX_CAPTURE_WAIT_S).contains(&wait_s) {
            return invalid_input(format!(
                "profile_capture: `max_wait_s` is {wait_s}; give 1 to {MAX_CAPTURE_WAIT_S} \
                 seconds."
            ));
        }
        let id = if let Some(id) = p.id {
            id
        } else {
            let started = match self
                .engine
                .call("profile.capture", Some(to_params(&p.settings())))
                .await
            {
                Ok(v) => v,
                Err(e) => return error_result(&e),
            };
            match serde_json::from_value::<CaptureStarted>(started.clone()) {
                Ok(s) => s.id,
                Err(_) => return json_result(&started),
            }
        };
        match self
            .poll_capture(id, Duration::from_secs(u64::from(wait_s)))
            .await
        {
            Ok(Polled::Done(summary)) => json_result(&summary),
            Ok(Polled::Pending(status)) => {
                CallToolResult::success(vec![ContentBlock::text(pending_text(&status))])
            }
            Err(result) => result,
        }
    }

    #[doc = include_str!("../../content/tools/profile_counters.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn profile_counters(&self) -> CallToolResult {
        self.run("profile.counters", None).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pending_text_says_how_to_continue() {
        let recording = CaptureStatus {
            id: 7,
            status: CaptureState::Recording,
            frames: 600,
            frames_done: 240,
            waiting_for_gpu: false,
        };
        let text = pending_text(&recording);
        assert!(text.starts_with("Capture 7 is still recording (240/600 frames)."));
        assert!(text.contains(r#"{"id":7}"#));

        let gpu = CaptureStatus {
            waiting_for_gpu: true,
            ..recording
        };
        assert!(pending_text(&gpu).contains("waiting for the last GPU timings"));
    }
}
