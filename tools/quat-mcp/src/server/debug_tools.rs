//! Debugging tools (ROADMAP A5): engine log, frame statistics, the
//! simulation clock (pause / step / time scale), entity watch, physics
//! raycasts and navmesh path queries. Descriptions live in
//! `content/tools/<tool>.md`; the engine methods are documented in
//! docs/remote-control.md ("Debugging methods").

use std::fmt::Write as _;
use std::time::Duration;

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde::Serialize;

use super::QuatServer;
use super::results::{error_result, invalid_input, json_result, to_params};
use crate::params::{
    DebugLogParams, DebugNavPathParams, DebugRaycastParams, DebugStepParams, DebugTimeScaleParams,
    DebugWatchParams, LogReply,
};

/// Most entities one `debug_watch` call may name (engine limit).
pub const MAX_WATCH_ENTITIES: usize = 64;

/// Extra reply time `debug_step` allows per stepped frame: the engine runs one
/// step per rendered frame and answers after the last one.
const STEP_TIME_PER_FRAME: Duration = Duration::from_millis(100);

/// The paging footer printed after the log lines.
#[derive(Serialize)]
struct LogFooter {
    cursor: u64,
    more: bool,
    missed: u64,
}

/// `debug.log`'s reply as readable text: one line per log line, then the
/// paging footer as JSON.
fn format_log(reply: &LogReply) -> String {
    let mut text = String::new();
    if reply.missed > 0 {
        let _ = writeln!(
            text,
            "({} older lines were overwritten before this read)",
            reply.missed
        );
    }
    if reply.lines.is_empty() {
        text.push_str("No matching log lines.\n");
    }
    for l in &reply.lines {
        let _ = writeln!(
            text,
            "#{} t={:.3} {} [{}] {}",
            l.seq, l.t, l.level, l.category, l.message
        );
    }
    let footer = LogFooter {
        cursor: reply.cursor,
        more: reply.more,
        missed: reply.missed,
    };
    text.push_str(&serde_json::to_string(&footer).unwrap_or_default());
    text
}

#[tool_router(router = debug_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/debug_log.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn debug_log(&self, params: Parameters<DebugLogParams>) -> CallToolResult {
        let value = match self
            .engine
            .call("debug.log", Some(to_params(&params.0)))
            .await
        {
            Ok(v) => v,
            Err(e) => return error_result(&e),
        };
        // An unexpected shape (a newer engine) is passed through untouched.
        match serde_json::from_value::<LogReply>(value.clone()) {
            Ok(reply) => CallToolResult::success(vec![ContentBlock::text(format_log(&reply))]),
            Err(_) => json_result(&value),
        }
    }

    #[doc = include_str!("../../content/tools/debug_stats.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn debug_stats(&self) -> CallToolResult {
        self.run("debug.stats", None).await
    }

    #[doc = include_str!("../../content/tools/debug_pause.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn debug_pause(&self) -> CallToolResult {
        self.run("debug.pause", None).await
    }

    #[doc = include_str!("../../content/tools/debug_resume.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn debug_resume(&self) -> CallToolResult {
        self.run("debug.resume", None).await
    }

    #[doc = include_str!("../../content/tools/debug_step.md")]
    #[tool]
    async fn debug_step(&self, params: Parameters<DebugStepParams>) -> CallToolResult {
        let frames = params.0.frames.unwrap_or(1);
        let timeout = self.long_timeout() + STEP_TIME_PER_FRAME * frames;
        match self
            .engine
            .call_with_timeout("debug.step", Some(to_params(&params.0)), timeout)
            .await
        {
            Ok(v) => json_result(&v),
            Err(e) => error_result(&e),
        }
    }

    #[doc = include_str!("../../content/tools/debug_time_scale.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn debug_time_scale(&self, params: Parameters<DebugTimeScaleParams>) -> CallToolResult {
        self.run("debug.time_scale", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/debug_watch.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn debug_watch(&self, params: Parameters<DebugWatchParams>) -> CallToolResult {
        let p = &params.0;
        match (&p.entity, &p.entities) {
            (Some(_), Some(_)) | (None, None) => {
                return invalid_input(
                    "debug_watch: give exactly one of `entity` (an id or unique name) or \
                     `entities` (an array of up to 64).",
                );
            }
            (None, Some(list)) if list.is_empty() || list.len() > MAX_WATCH_ENTITIES => {
                return invalid_input(format!(
                    "debug_watch: `entities` has {} entries; give 1 to {MAX_WATCH_ENTITIES}.",
                    list.len()
                ));
            }
            _ => {}
        }
        self.run("debug.watch", Some(to_params(p))).await
    }

    #[doc = include_str!("../../content/tools/debug_raycast.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn debug_raycast(&self, params: Parameters<DebugRaycastParams>) -> CallToolResult {
        if params.0.direction.iter().all(|c| *c == 0.0) {
            return invalid_input(
                "debug_raycast: `direction` is [0, 0, 0]; give the direction to cast in, e.g. \
                 [0, -1, 0] for straight down.",
            );
        }
        self.run("debug.raycast", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/debug_nav_path.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn debug_nav_path(&self, params: Parameters<DebugNavPathParams>) -> CallToolResult {
        self.run("debug.nav_path", Some(to_params(&params.0))).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::params::LogLine;

    #[test]
    fn log_lines_render_one_per_line_with_the_cursor() {
        let reply = LogReply {
            lines: vec![LogLine {
                seq: 7,
                t: 1.5,
                level: "warn".into(),
                category: "nav".into(),
                message: "agent stuck".into(),
            }],
            cursor: 9,
            more: false,
            missed: 2,
        };
        let text = format_log(&reply);
        assert!(text.starts_with("(2 older lines were overwritten"));
        assert!(text.contains("#7 t=1.500 warn [nav] agent stuck\n"));
        assert!(text.ends_with(r#"{"cursor":9,"more":false,"missed":2}"#));
    }

    #[test]
    fn an_empty_read_says_so() {
        let reply = LogReply {
            lines: vec![],
            cursor: 3,
            more: false,
            missed: 0,
        };
        assert!(format_log(&reply).starts_with("No matching log lines."));
    }
}
