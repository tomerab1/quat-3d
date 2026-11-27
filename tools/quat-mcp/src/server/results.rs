//! Turning engine replies and errors into MCP tool results.

use rmcp::model::{CallToolResult, ContentBlock};
use serde::Serialize;
use serde_json::{Value, json};

use crate::client::{EngineError, code, code_name};

pub(super) fn to_params<T: Serialize>(p: &T) -> Value {
    serde_json::to_value(p).unwrap_or_else(|_| json!({}))
}

/// A successful engine result as compact JSON text.
pub(super) fn json_result(v: &Value) -> CallToolResult {
    CallToolResult::success(vec![ContentBlock::text(v.to_string())])
}

/// What the model should do next after an engine error with this code.
fn hint(rpc_code: i64) -> Option<&'static str> {
    match rpc_code {
        code::INVALID_PARAMS => Some(
            "Fix the field named above and call again; the tool description lists every \
             component and its fields.",
        ),
        code::NOT_FOUND => Some(
            "Use scene_list (filter with name_contains) to find valid entity ids or names, and \
             asset_list_models for model paths.",
        ),
        code::UNAVAILABLE => Some(
            "The running engine cannot do this in its current build or state (e.g. it runs \
             without a window or editor); continue without it.",
        ),
        code::METHOD_NOT_FOUND => Some(
            "The running engine does not implement this method (yet); engine_methods lists \
             what it supports.",
        ),
        _ => None,
    }
}

/// A failed call as a tool error the model can read and act on.
pub fn error_result(e: &EngineError) -> CallToolResult {
    let mut text = e.to_string();
    if let Some(hint) = e.rpc_code().and_then(hint) {
        text.push_str("\nHint: ");
        text.push_str(hint);
    }
    if let EngineError::Rpc {
        data: Some(data), ..
    } = e
    {
        text.push_str("\nDetails: ");
        text.push_str(&data.to_string());
    }
    CallToolResult::error(vec![ContentBlock::text(text)])
}

pub(super) fn invalid_input(message: impl Into<String>) -> CallToolResult {
    CallToolResult::error(vec![ContentBlock::text(message.into())])
}

/// Map a tool-style method name ("`scene_create`") to the engine's ("scene.create").
#[must_use]
pub fn normalize_method(method: &str) -> String {
    let m = method.trim();
    if m.contains('.') {
        return m.to_owned();
    }
    if m == "screenshot" {
        return "render.screenshot".to_owned();
    }
    match m.split_once('_') {
        Some((ns, rest)) => format!("{ns}.{rest}"),
        None => m.to_owned(),
    }
}

/// Prefix a summary when some batch commands failed or were skipped.
pub(super) fn batch_summary(commands: usize, result: &Value) -> Option<String> {
    use std::fmt::Write as _;

    let results = result.get("results")?.as_array()?;
    let failed: Vec<(usize, &Value)> = results
        .iter()
        .enumerate()
        .filter(|(_, r)| r.get("ok") == Some(&Value::Bool(false)))
        .collect();
    let skipped = commands.saturating_sub(results.len());
    if failed.is_empty() && skipped == 0 {
        return None;
    }
    let mut s = format!("{} of {commands} commands failed", failed.len());
    if skipped > 0 {
        let _ = write!(s, ", {skipped} not run (stop_on_error)");
    }
    s.push('.');
    for (i, r) in failed.iter().take(5) {
        let err = r.get("error");
        let code = err
            .and_then(|e| e.get("code"))
            .and_then(Value::as_i64)
            .unwrap_or(0);
        let msg = err
            .and_then(|e| e.get("message"))
            .and_then(Value::as_str)
            .unwrap_or("unknown error");
        let _ = write!(s, "\n- command {i}: {} ({code}): {msg}", code_name(code));
    }
    if failed.len() > 5 {
        let _ = write!(s, "\n- … and {} more", failed.len() - 5);
    }
    Some(s)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn method_names_are_normalized() {
        assert_eq!(normalize_method("scene.create"), "scene.create");
        assert_eq!(normalize_method("scene_create"), "scene.create");
        assert_eq!(
            normalize_method("scene_remove_components"),
            "scene.remove_components"
        );
        assert_eq!(
            normalize_method("world_ground_height"),
            "world.ground_height"
        );
        assert_eq!(normalize_method(" screenshot "), "render.screenshot");
    }

    #[test]
    fn batch_summary_reports_failures_and_skips() {
        let ok = json!({"results": [{"ok": true, "result": {"id": 1}}]});
        assert!(batch_summary(1, &ok).is_none());
        let bad = json!({"results": [
            {"ok": true, "result": {"id": 1}},
            {"ok": false, "error": {"code": -32602, "message": "primitive.shape: unknown"}}
        ]});
        let s = batch_summary(3, &bad).unwrap();
        assert!(s.starts_with("1 of 3 commands failed, 1 not run"));
        assert!(s.contains("command 1: invalid_params (-32602): primitive.shape: unknown"));
    }
}
