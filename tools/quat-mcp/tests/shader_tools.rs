//! The material-shader tools (`shader_*`) and the `author-material-shader`
//! prompt, end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult, ContentBlock, GetPromptRequestParams};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const SHADER_TOOLS: [&str; 6] = [
    "shader_list",
    "shader_get",
    "shader_template",
    "shader_create",
    "shader_update",
    "shader_delete",
];

const TEMPLATE: &str = "import material;\nstruct MyMaterial : IMaterial { ... }\n\
                        export struct SurfaceMaterial : IMaterial = MyMaterial;\n";

/// A mock engine implementing the shader methods (docs/remote-control.md):
/// sources containing "uvv" fail to compile at line 9, column 33.
fn shader_handler(method: &str, params: &Value) -> Action {
    let name = params["name"].as_str().unwrap_or_default().to_owned();
    let path = format!("assets/shaders/materials/{name}.slang");
    match method {
        "shader.list" => Action::Reply(json!({"can_compile": true, "shaders": [
            {"name": "standard", "builtin": true, "path": "assets/shaders/lib/material.slang",
             "state": "builtin", "reloads": 0, "errors": 0},
            {"name": "lava", "builtin": false, "path": "assets/shaders/materials/lava.slang",
             "state": "ok", "reloads": 2, "errors": 0}]})),
        "shader.get" if name == "lava" => Action::Reply(json!({
            "name": "lava", "builtin": false, "path": path, "state": "ok", "reloads": 2,
            "errors": 0, "diagnostics": [], "log": "", "source": "// lava\nimport material;\n"})),
        "shader.get" => Action::Error(-32003, format!("no material shader \"{name}\"")),
        "shader.template" => Action::Reply(json!({"source": TEMPLATE,
            "save_as": "assets/shaders/materials/<name>.slang"})),
        // Deferred in the engine: the reply arrives once the compile finished.
        "shader.create" | "shader.update" => {
            let failed = params["source"].as_str().is_some_and(|s| s.contains("uvv"));
            let reply = if failed {
                json!({"name": name, "path": path, "ok": false, "errors": 1,
                       "kept_previous": method == "shader.update",
                       "log": format!("{path}(9): error 30027: 'uvv' is not a member of 'MaterialInput'."),
                       "diagnostics": [{"severity": "error", "file": path, "line": 9,
                           "column": 33, "code": 30027,
                           "message": "'uvv' is not a member of 'MaterialInput'.",
                           "source_line": "        s.albedo = float3(input.uvv, 0.0);"}]})
            } else {
                json!({"name": name, "path": path, "ok": true, "errors": 0,
                       "diagnostics": [], "log": ""})
            };
            Action::Delayed(Duration::from_millis(150), reply)
        }
        "shader.delete" => Action::Reply(json!({"deleted": name, "entities_reset": 3})),
        _ => Action::Error(-32601, format!("unknown method '{method}'")),
    }
}

async fn connect(engine_addr: &str) -> RunningService<RoleClient, ()> {
    let mut cfg = EngineConfig::new(engine_addr);
    cfg.timeout = Duration::from_secs(5);
    let server = QuatServer::new(Arc::new(EngineClient::new(cfg)), Duration::from_secs(10));
    let (server_io, client_io) = tokio::io::duplex(1 << 20);
    tokio::spawn(async move {
        let running = server.serve(server_io).await.expect("server starts");
        let _ = running.waiting().await;
    });
    ().serve(client_io).await.expect("client connects")
}

async fn call(client: &RunningService<RoleClient, ()>, tool: &str, args: Value) -> CallToolResult {
    let mut req = CallToolRequestParams::new(tool.to_owned());
    if let Value::Object(map) = args {
        req = req.with_arguments(map);
    }
    client.call_tool(req).await.expect("tools/call succeeds")
}

fn texts(result: &CallToolResult) -> Vec<String> {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect()
}

fn text(result: &CallToolResult) -> String {
    texts(result).join("\n")
}

#[tokio::test]
async fn shader_tools_and_prompt_are_listed() {
    let engine = MockEngine::start(None, shader_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in SHADER_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let create = tools.iter().find(|t| t.name == "shader_create").unwrap();
    let schema = serde_json::to_string(&create.input_schema).unwrap();
    assert!(schema.contains("\"name\"") && schema.contains("\"source\""));
    let delete = tools.iter().find(|t| t.name == "shader_delete").unwrap();
    assert_eq!(
        delete.annotations.as_ref().and_then(|a| a.destructive_hint),
        Some(true)
    );

    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "author-material-shader"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn author_material_shader_prompt_walks_the_shader_workflow() {
    let engine = MockEngine::start(None, shader_handler).await;
    let client = connect(&engine.addr_string()).await;
    let args = json!({"goal": "glowing lava with flowing cracks", "entity": "Rock"});
    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("author-material-shader")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(t.text.contains("glowing lava with flowing cracks"));
    assert!(t.text.contains("The surface: Rock"));
    // Shader workflow order: template -> create -> fix from diagnostics ->
    // assign -> screenshot -> update.
    let shader = &t.text[t
        .text
        .find("## Workflow: material shader")
        .expect("the skill's shader workflow")..];
    let at = |s: &str| shader.find(s).unwrap_or_else(|| panic!("missing {s}"));
    assert!(at("shader_template") < at("shader_create"));
    assert!(at("shader_create") < at("file:line:column"));
    assert!(at("file:line:column") < at("scene_update"));
    assert!(at("scene_update") < at("screenshot"));
    assert!(at("screenshot") < at("shader_update"));
    assert!(!t.text.contains("{%") && !t.text.contains("{{"));

    // The goal is required.
    assert!(
        client
            .get_prompt(GetPromptRequestParams::new("author-material-shader"))
            .await
            .is_err()
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn create_reports_success_and_compiler_errors_with_locations() {
    let engine = MockEngine::start(None, shader_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "shader_create",
        json!({"name": "lava", "source": TEMPLATE}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("it is live"), "{}", text(&r));
    assert_eq!(
        engine.last_params("shader.create"),
        Some(json!({"name": "lava", "source": TEMPLATE}))
    );

    let broken = "import material;\n// ...\n        s.albedo = float3(input.uvv, 0.0);\n";
    let r = call(
        &client,
        "shader_create",
        json!({"name": "lava2", "source": broken}),
    )
    .await;
    assert_eq!(
        r.is_error,
        Some(true),
        "a failed compile is an error result"
    );
    let out = text(&r);
    assert!(out.contains("nothing was saved"), "{out}");
    assert!(
        out.contains(
            "assets/shaders/materials/lava2.slang:9:33: error[30027]: 'uvv' is not a member of \
             'MaterialInput'."
        ),
        "{out}"
    );
    assert!(
        out.contains(" 9 |         s.albedo = float3(input.uvv, 0.0);"),
        "{out}"
    );
    assert!(out.contains('^'), "{out}");

    let r = call(
        &client,
        "shader_update",
        json!({"name": "lava", "source": broken}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("previous version was restored and keeps running"),
        "{}",
        text(&r)
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn bad_names_and_sources_are_rejected_before_the_engine() {
    let engine = MockEngine::start(None, shader_handler).await;
    let client = connect(&engine.addr_string()).await;
    for (tool, args) in [
        (
            "shader_create",
            json!({"name": "../evil", "source": TEMPLATE}),
        ),
        ("shader_create", json!({"name": "Lava", "source": TEMPLATE})),
        ("shader_update", json!({"name": "lava", "source": "   "})),
        ("shader_delete", json!({"name": "a/b"})),
    ] {
        let r = call(&client, tool, args.clone()).await;
        assert_eq!(r.is_error, Some(true), "{tool} {args}");
    }
    let huge = "x".repeat(256 * 1024 + 1);
    let r = call(
        &client,
        "shader_create",
        json!({"name": "big", "source": huge}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("limited to"));
    assert!(
        !engine.methods().iter().any(|m| m.starts_with("shader.")),
        "{:?}",
        engine.methods()
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn list_get_template_and_delete() {
    let engine = MockEngine::start(None, shader_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "shader_list", json!({})).await;
    assert!(text(&r).contains(r#""name":"lava""#), "{}", text(&r));

    // Metadata first, then the source verbatim (ready to edit and send back).
    let r = call(&client, "shader_get", json!({"name": "lava"})).await;
    assert_eq!(r.is_error, Some(false));
    let blocks = texts(&r);
    assert_eq!(blocks.len(), 2);
    let meta: Value = serde_json::from_str(&blocks[0]).unwrap();
    assert_eq!(meta["state"], "ok");
    assert!(meta.get("source").is_none());
    assert_eq!(blocks[1], "// lava\nimport material;\n");

    let r = call(&client, "shader_get", json!({"name": "nope"})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("not_found"), "{}", text(&r));

    let r = call(&client, "shader_template", json!({})).await;
    assert_eq!(text(&r), TEMPLATE);

    let r = call(&client, "shader_delete", json!({"name": "lava"})).await;
    assert!(text(&r).contains(r#""entities_reset":3"#));
    assert_eq!(
        engine.last_params("shader.delete"),
        Some(json!({"name": "lava"}))
    );
    client.cancel().await.unwrap();
}
