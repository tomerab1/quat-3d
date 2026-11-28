//! The debugging tools (`debug_*`) and the `debug-session` prompt, end to
//! end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult, ContentBlock, GetPromptRequestParams};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const DEBUG_TOOLS: [&str; 9] = [
    "debug_log",
    "debug_stats",
    "debug_pause",
    "debug_resume",
    "debug_step",
    "debug_time_scale",
    "debug_watch",
    "debug_raycast",
    "debug_nav_path",
];

fn clock(paused: bool, steps_run: u64) -> Value {
    json!({"paused": paused, "time_scale": 1.0, "pending_steps": 0, "steps_run": steps_run,
           "sim_frames": steps_run, "sim_time_s": 0.5})
}

/// A mock engine implementing the debug methods (docs/remote-control.md).
fn debug_handler(method: &str, params: &Value) -> Action {
    match method {
        "debug.log" => Action::Reply(json!({
            "lines": [
                {"seq": 4, "t": 1.25, "level": "warn", "category": "nav",
                 "message": "agent 12 gave up"},
                {"seq": 5, "t": 2.0, "level": "error", "category": "vulkan",
                 "message": "VUID-vkCmdDraw-None-02699"}
            ],
            "cursor": 5, "more": false, "missed": 0, "oldest_seq": 1, "capacity": 4096
        })),
        "debug.stats" => Action::Reply(json!({
            "frame": 900, "frame_time_ms": {"last": 16.6, "avg": 16.7, "worst": 41.0,
            "samples": 240}, "fps": 59.9, "draws": 120, "entities": 42,
            "physics": null, "nav": {"agents": 2, "moving": 1, "failed": 1,
            "navmesh_polygons": 310}, "memory": null, "sim": clock(false, 0),
            "log": {"last_seq": 5, "warnings": 1, "errors": 1, "last_error_seq": 5}
        })),
        "debug.pause" => Action::Reply(clock(true, 0)),
        "debug.resume" => Action::Reply(clock(false, 0)),
        // Deferred in the engine: the reply arrives after the steps ran.
        "debug.step" => Action::Delayed(
            Duration::from_millis(150),
            clock(true, params["frames"].as_u64().unwrap_or(1)),
        ),
        "debug.time_scale" => Action::Reply(json!({"time_scale": params["scale"]})),
        "debug.watch" => Action::Reply(json!({"entities": [{"id": 7, "name": "Crate",
            "world_position": [0, 0.5, 0], "components": {"transform": {}}}], "frame": 900})),
        "debug.raycast" => Action::Error(
            -32002,
            "no physics world is running: physics bodies exist only in play mode (sim.play)".into(),
        ),
        "debug.nav_path" => Action::Reply(json!({"points": [[0, 0, 0], [5, 0, 5]],
            "partial": true, "length": 7.07})),
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
async fn debug_tools_and_prompt_are_listed() {
    let engine = MockEngine::start(None, debug_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in DEBUG_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let watch = tools.iter().find(|t| t.name == "debug_watch").unwrap();
    let schema = serde_json::to_string(&watch.input_schema).unwrap();
    assert!(schema.contains("entities") && schema.contains("components"));

    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "debug-session"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn debug_session_prompt_walks_the_workflow() {
    let engine = MockEngine::start(None, debug_handler).await;
    let client = connect(&engine.addr_string()).await;
    let args = json!({"problem": "the crate falls through the floor", "entity": "Crate"});
    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("debug-session")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(
        t.text
            .contains("The problem: the crate falls through the floor")
    );
    assert!(t.text.contains("Start with this entity: Crate"));
    // Workflow order: stats -> logs -> watch -> pause/step -> raycast.
    let at = |s: &str| t.text.find(s).unwrap_or_else(|| panic!("missing {s}"));
    assert!(at("debug_stats") < at("debug_log"));
    assert!(at("debug_log") < at("debug_watch"));
    assert!(at("debug_watch") < at("debug_step"));
    assert!(at("debug_step") < at("debug_raycast"));

    // Without arguments the optional blocks disappear.
    let bare = client
        .get_prompt(GetPromptRequestParams::new("debug-session"))
        .await
        .unwrap();
    let ContentBlock::Text(b) = &bare.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(!b.text.contains("The problem:") && !b.text.contains("{%"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn debug_log_formats_lines_and_forwards_filters() {
    let engine = MockEngine::start(None, debug_handler).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(
        &client,
        "debug_log",
        json!({"since_seq": 3, "level": "warn", "category": "nav"}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let out = text(&r);
    assert!(
        out.contains("#4 t=1.250 warn [nav] agent 12 gave up"),
        "{out}"
    );
    assert!(out.contains("#5 t=2.000 error [vulkan] VUID-vkCmdDraw-None-02699"));
    assert!(
        out.ends_with(r#"{"cursor":5,"more":false,"missed":0}"#),
        "{out}"
    );
    assert_eq!(
        engine.last_params("debug.log"),
        Some(json!({"since_seq": 3, "level": "warn", "category": "nav"}))
    );

    // A bad level is rejected before it reaches the engine (as a protocol
    // error or an error result, depending on the rmcp version).
    let before = engine.methods().len();
    let mut req = CallToolRequestParams::new("debug_log");
    req = req.with_arguments(json!({"level": "loud"}).as_object().unwrap().clone());
    let rejected = client
        .call_tool(req)
        .await
        .map_or(true, |r| r.is_error == Some(true));
    assert!(rejected);
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn clock_tools_forward_and_step_waits_for_the_reply() {
    let engine = MockEngine::start(None, debug_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "debug_pause", json!({})).await;
    assert!(text(&r).contains(r#""paused":true"#), "{}", text(&r));
    let r = call(&client, "debug_step", json!({"frames": 3})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""steps_run":3"#), "{}", text(&r));
    assert_eq!(engine.last_params("debug.step"), Some(json!({"frames": 3})));
    let r = call(&client, "debug_time_scale", json!({"scale": 0.25})).await;
    assert!(text(&r).contains("0.25"));
    let r = call(&client, "debug_resume", json!({})).await;
    assert!(text(&r).contains(r#""paused":false"#));
    let r = call(&client, "debug_stats", json!({})).await;
    assert!(text(&r).contains(r#""failed":1"#));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn watch_raycast_and_nav_path() {
    let engine = MockEngine::start(None, debug_handler).await;
    let client = connect(&engine.addr_string()).await;

    // Client-side checks answer without a round trip.
    let r = call(&client, "debug_watch", json!({})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("exactly one of"));
    let r = call(
        &client,
        "debug_watch",
        json!({"entity": 1, "entities": [2]}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    let r = call(
        &client,
        "debug_raycast",
        json!({"origin": [0, 1, 0], "direction": [0, 0, 0]}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(!engine.methods().iter().any(|m| m.starts_with("debug.")));

    let r = call(
        &client,
        "debug_watch",
        json!({"entity": "Crate", "components": ["transform"]}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("debug.watch"),
        Some(json!({"entity": "Crate", "components": ["transform"]}))
    );

    // Engine errors keep their code and message, plus a hint.
    let r = call(
        &client,
        "debug_raycast",
        json!({"origin": [0, 1, 0], "direction": [0, -1, 0]}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    let out = text(&r);
    assert!(
        out.contains("unavailable") && out.contains("sim.play"),
        "{out}"
    );

    let r = call(
        &client,
        "debug_nav_path",
        json!({"from": [0, 0, 0], "to": [9, 0, 9]}),
    )
    .await;
    assert!(text(&r).contains(r#""partial":true"#));
    client.cancel().await.unwrap();
}
