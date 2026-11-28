//! The gameplay script tools (`script_*`), the scripting reference resource
//! and the `write-gameplay-script` prompt, end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{
    CallToolRequestParams, CallToolResult, ContentBlock, GetPromptRequestParams,
    ReadResourceRequestParams, ResourceContents,
};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const SCRIPT_TOOLS: [&str; 6] = [
    "script_list",
    "script_get",
    "script_create",
    "script_update",
    "script_delete",
    "script_check",
];

/// A mock engine implementing the script methods (docs/remote-control.md).
fn script_handler(method: &str, params: &Value) -> Action {
    match method {
        "script.list" => Action::Reply(json!({"running": true, "scripts": [
            {"path": "coin.lua", "bytes": 120, "entities": 8,
             "status": {"state": "error", "message": "coin.lua:7: attempt to index a nil value",
                        "instances": 7, "failed": 1, "reloads": 2}}]})),
        "script.get" => Action::Reply(json!({"path": params["path"],
            "source": "return {\n  properties = { value = 1 },\n}", "entities": 8,
            "status": null})),
        "script.create" | "script.update" => {
            let source = params["source"].as_str().unwrap_or_default();
            if source.contains("= =") {
                Action::ErrorWithData(
                    -32602,
                    "syntax error at line 2: coin.lua:2: unexpected symbol near '='".into(),
                    json!({"path": params["path"], "line": 2}),
                )
            } else {
                Action::Reply(json!({"path": params["path"], "bytes": source.len(),
                    "entities": 0, "running": false}))
            }
        }
        "script.delete" => Action::Reply(
            json!({"path": params["path"], "deleted": true, "entities_still_using": 1}),
        ),
        "script.check" => Action::Reply(json!({"ok": true})),
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
    common::client(client_io).await
}

async fn call(client: &RunningService<RoleClient, ()>, tool: &str, args: Value) -> CallToolResult {
    let mut req = CallToolRequestParams::new(tool.to_owned());
    if let Value::Object(map) = args {
        req = req.with_arguments(map);
    }
    client.call_tool(req).await.expect("tools/call succeeds")
}

fn text(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect::<Vec<_>>()
        .join("\n")
}

#[tokio::test]
async fn script_tools_prompt_and_resource_are_listed() {
    let engine = MockEngine::start(None, script_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in SCRIPT_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let create = tools.iter().find(|t| t.name == "script_create").unwrap();
    let schema = serde_json::to_string(&create.input_schema).unwrap();
    assert!(schema.contains("source") && schema.contains("assets/scripts/"));

    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "write-gameplay-script"));
    let resources = client.list_all_resources().await.unwrap();
    assert!(resources.iter().any(|r| r.uri == "quat://docs/scripting"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn the_scripting_reference_is_a_resource() {
    let engine = MockEngine::start(None, script_handler).await;
    let client = connect(&engine.addr_string()).await;
    let doc = client
        .read_resource(ReadResourceRequestParams::new("quat://docs/scripting"))
        .await
        .unwrap();
    let ResourceContents::TextResourceContents {
        text, mime_type, ..
    } = &doc.contents[0]
    else {
        panic!("text resource expected");
    };
    assert_eq!(mime_type.as_deref(), Some("text/markdown"));
    for needle in [
        "on_update",
        "on_trigger_enter",
        "scene.spawn",
        "input.move_vector",
    ] {
        assert!(text.contains(needle), "the reference documents {needle}");
    }
    assert!(engine.methods().is_empty(), "served without the engine");
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn create_update_get_and_delete_forward_to_the_engine() {
    let engine = MockEngine::start(None, script_handler).await;
    let client = connect(&engine.addr_string()).await;

    let source = "local M = {}\nfunction M:on_update(dt) end\nreturn M";
    let r = call(
        &client,
        "script_create",
        json!({"path": "coin.lua", "source": source}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("script.create"),
        Some(json!({"path": "coin.lua", "source": source}))
    );

    let r = call(
        &client,
        "script_update",
        json!({"path": " coin.lua ", "source": source}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("script.update").unwrap()["path"],
        "coin.lua"
    );

    let r = call(&client, "script_get", json!({"path": "coin.lua"})).await;
    let out = text(&r);
    assert!(out.contains(r#""entities":8"#), "{out}");
    assert!(
        out.ends_with("```lua\nreturn {\n  properties = { value = 1 },\n}\n```"),
        "{out}"
    );

    let r = call(&client, "script_list", json!({})).await;
    assert!(text(&r).contains("coin.lua:7: attempt to index a nil value"));

    let r = call(
        &client,
        "script_delete",
        json!({"path": "coin.lua", "confirm": true}),
    )
    .await;
    assert!(text(&r).contains(r#""entities_still_using":1"#));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn syntax_errors_name_the_line_and_bad_input_stays_local() {
    let engine = MockEngine::start(None, script_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "script_update",
        json!({"path": "coin.lua", "source": "local x\nx = = 1"}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    let out = text(&r);
    assert!(
        out.contains("coin.lua:2:") && out.contains("Fix line 2"),
        "{out}"
    );
    assert!(out.contains("Nothing was saved"));

    // Answered without a round trip: bad paths, empty or oversized sources.
    let before = engine.methods().len();
    for (tool, args) in [
        (
            "script_create",
            json!({"path": "../escape.lua", "source": "x = 1"}),
        ),
        (
            "script_create",
            json!({"path": "notes.txt", "source": "x = 1"}),
        ),
        (
            "script_create",
            json!({"path": "assets/scripts/coin.lua", "source": "x = 1"}),
        ),
        (
            "script_update",
            json!({"path": "coin.lua", "source": "   "}),
        ),
        (
            "script_update",
            json!({"path": "coin.lua", "source": "x".repeat(300 * 1024)}),
        ),
        ("script_get", json!({"path": "/etc/passwd"})),
        (
            "script_delete",
            json!({"path": "a//b.lua", "confirm": true}),
        ),
    ] {
        let r = call(&client, tool, args).await;
        assert_eq!(r.is_error, Some(true), "{tool}");
    }
    assert_eq!(engine.methods().len(), before);

    let r = call(&client, "script_check", json!({"source": "return 1"})).await;
    assert!(text(&r).contains(r#""ok":true"#));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn write_gameplay_script_prompt_walks_the_workflow() {
    let engine = MockEngine::start(None, script_handler).await;
    let client = connect(&engine.addr_string()).await;
    let args = json!({"goal": "a door that opens when the player is near",
                      "entity": "Door", "path": "door.lua"});
    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("write-gameplay-script")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(t.text.contains("a door that opens when the player is near"));
    assert!(t.text.contains("Attach it to this entity: Door"));
    assert!(t.text.contains("assets/scripts/door.lua"));
    // Workflow order: reference -> write -> attach -> play -> log -> iterate.
    let at = |s: &str| t.text.find(s).unwrap_or_else(|| panic!("missing {s}"));
    assert!(at("quat://docs/scripting") < at("script_create"));
    assert!(at("script_create") < at("scene_update"));
    assert!(at("scene_update") < at("sim_play"));
    assert!(at("sim_play") < at("debug_log"));
    assert!(at("debug_log") < at("script_update` hot-reloads"));

    let bare = client
        .get_prompt(
            GetPromptRequestParams::new("write-gameplay-script")
                .with_arguments(json!({"goal": "x"}).as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(b) = &bare.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(!b.text.contains("Attach it to this entity") && !b.text.contains("{%"));
    client.cancel().await.unwrap();
}
