//! Material shader tools (ROADMAP A6): list, read, create, edit and delete
//! the Slang material modules the engine compiles and hot-swaps. Compile
//! results are rendered like compiler output (`file:line:column: error ...`
//! with the source line and a caret) so the model can go straight to the
//! fix. Descriptions live in `content/tools/<tool>.md`; the engine methods are
//! documented in docs/remote-control.md ("Material shaders").

use std::fmt::Write as _;

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde_json::Value;

use super::QuatServer;
use super::results::{error_result, invalid_input, json_result, to_params};
use crate::params::{
    CompileOutcome, Diagnostic, MAX_SHADER_SOURCE, ShaderNameParams, ShaderSourceParams,
    shader_name_problem,
};

/// One diagnostic as compiler-style text.
fn format_diagnostic(d: &Diagnostic) -> String {
    let mut s = String::new();
    match (&d.file, d.line, d.column) {
        (Some(file), Some(line), Some(col)) => {
            let _ = write!(s, "{file}:{line}:{col}: ");
        }
        (Some(file), Some(line), None) => {
            let _ = write!(s, "{file}:{line}: ");
        }
        (Some(file), None, _) if !file.is_empty() => {
            let _ = write!(s, "{file}: ");
        }
        _ => {}
    }
    s.push_str(&d.severity);
    if let Some(code) = d.code {
        let _ = write!(s, "[{code}]");
    }
    let _ = write!(s, ": {}", d.message);
    if let (Some(src), Some(line)) = (&d.source_line, d.line) {
        let gutter = line.to_string();
        let _ = write!(s, "\n {gutter} | {src}");
        if let Some(col) = d.column {
            let pad = " ".repeat(gutter.len());
            let caret = " ".repeat(col.saturating_sub(1) as usize);
            let _ = write!(s, "\n {pad} | {caret}^");
        }
    }
    s
}

/// The compile outcome as text: a one-line verdict, then the diagnostics.
fn format_outcome(o: &CompileOutcome, created: bool) -> String {
    let mut text = if o.ok {
        format!(
            "Compiled material shader `{}` ({}); it is live: entities whose primitive.shader is \
             \"{}\" now render with it.",
            o.name, o.path, o.name
        )
    } else if created {
        format!(
            "Compiling `{}` failed, so nothing was saved. Fix the errors below and call \
             shader_create again.",
            o.name
        )
    } else if o.kept_previous {
        format!(
            "Compiling the new `{}` failed; the previous version was restored and keeps \
             running. Fix the errors below and call shader_update again.",
            o.name
        )
    } else {
        format!(
            "Compiling the new `{}` failed. Fix the errors below.",
            o.name
        )
    };
    for d in &o.diagnostics {
        text.push_str("\n\n");
        text.push_str(&format_diagnostic(d));
    }
    let located = o.diagnostics.iter().any(|d| d.line.is_some());
    if !o.ok && !located && !o.log.trim().is_empty() {
        let _ = write!(text, "\n\nCompiler output:\n{}", o.log.trim());
    }
    text
}

impl QuatServer {
    async fn write_shader(&self, method: &str, p: &ShaderSourceParams) -> CallToolResult {
        if let Some(problem) = shader_name_problem(&p.name) {
            return invalid_input(problem);
        }
        if p.source.trim().is_empty() {
            return invalid_input(
                "`source` is empty; send the whole Slang module (start from shader_template).",
            );
        }
        if p.source.len() > MAX_SHADER_SOURCE {
            return invalid_input(format!(
                "`source` is {} bytes; material shaders are limited to {MAX_SHADER_SOURCE}.",
                p.source.len()
            ));
        }
        // The engine replies once the compile has finished.
        let reply = match self
            .engine
            .call_with_timeout(method, Some(to_params(p)), self.long_timeout())
            .await
        {
            Ok(v) => v,
            Err(e) => return error_result(&e),
        };
        match serde_json::from_value::<CompileOutcome>(reply.clone()) {
            Ok(outcome) => {
                let text = format_outcome(&outcome, method == "shader.create");
                if outcome.ok {
                    CallToolResult::success(vec![ContentBlock::text(text)])
                } else {
                    CallToolResult::error(vec![ContentBlock::text(text)])
                }
            }
            // An unexpected shape (a newer engine) is passed through untouched.
            Err(_) => json_result(&reply),
        }
    }
}

#[tool_router(router = shader_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/shader_list.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn shader_list(&self) -> CallToolResult {
        self.run("shader.list", None).await
    }

    #[doc = include_str!("../../content/tools/shader_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn shader_get(&self, params: Parameters<ShaderNameParams>) -> CallToolResult {
        let mut reply = match self
            .engine
            .call("shader.get", Some(to_params(&params.0)))
            .await
        {
            Ok(v) => v,
            Err(e) => return error_result(&e),
        };
        // Metadata as JSON, the module text verbatim (ready to edit and send back).
        let source = reply
            .as_object_mut()
            .and_then(|m| m.remove("source"))
            .and_then(|s| s.as_str().map(str::to_owned));
        let mut blocks = vec![ContentBlock::text(reply.to_string())];
        if let Some(source) = source {
            blocks.push(ContentBlock::text(source));
        }
        CallToolResult::success(blocks)
    }

    #[doc = include_str!("../../content/tools/shader_template.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn shader_template(&self) -> CallToolResult {
        match self.engine.call("shader.template", None).await {
            Ok(v) => match v.get("source").and_then(Value::as_str) {
                Some(source) => CallToolResult::success(vec![ContentBlock::text(source)]),
                None => json_result(&v),
            },
            Err(e) => error_result(&e),
        }
    }

    #[doc = include_str!("../../content/tools/shader_create.md")]
    #[tool]
    async fn shader_create(&self, params: Parameters<ShaderSourceParams>) -> CallToolResult {
        self.write_shader("shader.create", &params.0).await
    }

    #[doc = include_str!("../../content/tools/shader_update.md")]
    #[tool]
    async fn shader_update(&self, params: Parameters<ShaderSourceParams>) -> CallToolResult {
        self.write_shader("shader.update", &params.0).await
    }

    #[doc = include_str!("../../content/tools/shader_delete.md")]
    #[tool(annotations(destructive_hint = true))]
    async fn shader_delete(&self, params: Parameters<ShaderNameParams>) -> CallToolResult {
        if let Some(problem) = shader_name_problem(&params.0.name) {
            return invalid_input(problem);
        }
        self.run("shader.delete", Some(to_params(&params.0))).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn error_at_line_9() -> Diagnostic {
        Diagnostic {
            severity: "error".into(),
            message: "'uvv' is not a member of 'MaterialInput'.".into(),
            file: Some("assets/shaders/materials/lava.slang".into()),
            line: Some(9),
            column: Some(33),
            code: Some(30027),
            source_line: Some("        s.albedo = float3(input.uvv, 0.0);".into()),
        }
    }

    #[test]
    fn diagnostics_render_like_compiler_output() {
        let text = format_diagnostic(&error_at_line_9());
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(
            lines[0],
            "assets/shaders/materials/lava.slang:9:33: error[30027]: 'uvv' is not a member of \
             'MaterialInput'."
        );
        assert_eq!(lines[1], " 9 |         s.albedo = float3(input.uvv, 0.0);");
        // The caret sits under column 33 of the source line.
        let caret = lines[2].find('^').unwrap();
        let source_start = lines[1].find("| ").unwrap() + 2;
        assert_eq!(caret - source_start, 32);
        assert_eq!(&lines[1][caret..caret + 3], "uvv");
    }

    #[test]
    fn unlocated_diagnostics_and_failures_without_lines_show_the_log() {
        let outcome = CompileOutcome {
            name: "orphan".into(),
            ok: false,
            diagnostics: vec![Diagnostic {
                severity: "note".into(),
                message: "the module must end with `export struct SurfaceMaterial ...`".into(),
                ..Diagnostic::default()
            }],
            log: "(0): error 45001: unresolved external symbol".into(),
            ..CompileOutcome::default()
        };
        let text = format_outcome(&outcome, true);
        assert!(text.starts_with("Compiling `orphan` failed, so nothing was saved."));
        assert!(text.contains("\n\nnote: the module must end with"));
        assert!(text.ends_with("Compiler output:\n(0): error 45001: unresolved external symbol"));
    }

    #[test]
    fn verdicts_say_what_is_running() {
        let mut o = CompileOutcome {
            name: "lava".into(),
            path: "assets/shaders/materials/lava.slang".into(),
            ok: true,
            ..CompileOutcome::default()
        };
        assert!(format_outcome(&o, true).contains("it is live"));
        o.ok = false;
        o.kept_previous = true;
        o.diagnostics = vec![error_at_line_9()];
        let text = format_outcome(&o, false);
        assert!(text.contains("previous version was restored and keeps running"));
        assert!(text.contains("lava.slang:9:33"));
        assert!(!text.contains("Compiler output"));
    }
}
