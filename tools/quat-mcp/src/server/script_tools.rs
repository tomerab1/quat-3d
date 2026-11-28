//! Gameplay script tools (ROADMAP G7): list, read, write, delete and
//! compile-check Lua scripts under `assets/scripts/`. Descriptions live in
//! `content/tools/<tool>.md`; the Lua API is the `quat://docs/scripting`
//! resource (docs/scripting.md).

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde_json::{Value, json};

use super::QuatServer;
use super::results::{error_result, invalid_input, json_result, to_params};
use crate::client::{EngineError, code};
use crate::params::{
    ScriptCheckParams, ScriptPathParams, ScriptWriteParams, script_path_problem,
    script_source_problem,
};

/// A compile error from `script.create` / `script.update` (the engine attaches
/// `{path, line}`) as an actionable tool error; other errors as usual.
fn write_error(e: &EngineError, tool: &str) -> CallToolResult {
    if let EngineError::Rpc {
        code: code::INVALID_PARAMS,
        message,
        data: Some(data),
        ..
    } = e
        && let Some(line) = data.get("line").and_then(Value::as_u64)
    {
        return invalid_input(format!(
            "{message}\nNothing was saved. Fix line {line} (script_check compiles a draft \
             without saving) and call {tool} again."
        ));
    }
    error_result(e)
}

/// `script.get`'s reply as text: the metadata as JSON, then the source in a
/// fenced Lua block (easier to read and quote than an escaped JSON string).
fn format_script(reply: &Value) -> String {
    let mut meta = reply.clone();
    let source = meta
        .as_object_mut()
        .and_then(|m| m.remove("source"))
        .and_then(|s| s.as_str().map(str::to_owned));
    match source {
        Some(source) => format!("{meta}\n\n```lua\n{source}\n```"),
        None => reply.to_string(),
    }
}

#[tool_router(router = script_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/script_list.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn script_list(&self) -> CallToolResult {
        self.run("script.list", None).await
    }

    #[doc = include_str!("../../content/tools/script_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn script_get(&self, params: Parameters<ScriptPathParams>) -> CallToolResult {
        if let Some(problem) = script_path_problem(&params.0.path) {
            return invalid_input(format!("script_get: {problem}"));
        }
        match self
            .engine
            .call("script.get", Some(to_params(&params.0)))
            .await
        {
            Ok(v) => CallToolResult::success(vec![ContentBlock::text(format_script(&v))]),
            Err(e) => error_result(&e),
        }
    }

    #[doc = include_str!("../../content/tools/script_create.md")]
    #[tool]
    async fn script_create(&self, params: Parameters<ScriptWriteParams>) -> CallToolResult {
        self.write_script("script.create", "script_create", &params.0)
            .await
    }

    #[doc = include_str!("../../content/tools/script_update.md")]
    #[tool]
    async fn script_update(&self, params: Parameters<ScriptWriteParams>) -> CallToolResult {
        self.write_script("script.update", "script_update", &params.0)
            .await
    }

    #[doc = include_str!("../../content/tools/script_delete.md")]
    #[tool(annotations(destructive_hint = true))]
    async fn script_delete(&self, params: Parameters<ScriptPathParams>) -> CallToolResult {
        if let Some(problem) = script_path_problem(&params.0.path) {
            return invalid_input(format!("script_delete: {problem}"));
        }
        self.run("script.delete", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/script_check.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn script_check(&self, params: Parameters<ScriptCheckParams>) -> CallToolResult {
        if let Some(problem) = script_source_problem(&params.0.source) {
            return invalid_input(format!("script_check: {problem}"));
        }
        self.run("script.check", Some(to_params(&params.0))).await
    }
}

impl QuatServer {
    async fn write_script(
        &self,
        method: &str,
        tool: &str,
        params: &ScriptWriteParams,
    ) -> CallToolResult {
        if let Some(problem) =
            script_path_problem(&params.path).or_else(|| script_source_problem(&params.source))
        {
            return invalid_input(format!("{tool}: {problem}"));
        }
        let request = json!({"path": params.path.trim(), "source": params.source});
        match self.engine.call(method, Some(request)).await {
            Ok(v) => json_result(&v),
            Err(e) => write_error(&e, tool),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scripts_render_as_metadata_and_a_lua_block() {
        let text = format_script(&json!({
            "path": "coin.lua", "source": "return {}", "entities": 2, "status": null
        }));
        assert!(text.starts_with('{') && text.contains(r#""entities":2"#));
        assert!(!text.contains("\"source\""));
        assert!(text.ends_with("```lua\nreturn {}\n```"));
    }

    #[test]
    fn compile_errors_point_at_the_line() {
        let e = EngineError::Rpc {
            method: "script.update".into(),
            code: code::INVALID_PARAMS,
            message: "syntax error at line 3: coin.lua:3: '=' expected near 'x'".into(),
            data: Some(json!({"path": "coin.lua", "line": 3})),
        };
        let r = write_error(&e, "script_update");
        assert_eq!(r.is_error, Some(true));
        let text = r.content[0].as_text().unwrap().text.clone();
        assert!(
            text.contains("coin.lua:3:") && text.contains("Fix line 3"),
            "{text}"
        );
    }
}
