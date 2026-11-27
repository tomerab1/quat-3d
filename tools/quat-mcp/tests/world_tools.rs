//! World-building tools (ROADMAP A9) end to end against the mock engine:
//! what each tool sends and how it reports the reply.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

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

fn text(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect::<Vec<_>>()
        .join("\n")
}

/// Replies with the request's params, so tests see exactly what was sent.
async fn echo_engine() -> MockEngine {
    MockEngine::start(None, |_, params| Action::Reply(params.clone())).await
}

#[tokio::test]
async fn camera_set_sends_clip_planes_and_cut() {
    let engine = echo_engine().await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "camera_set",
        json!({"position": [0, 2, 0], "near": 0.05, "far": "auto", "cut": false}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("camera.set"),
        Some(json!({"position": [0.0, 2.0, 0.0], "near": 0.05, "far": null, "cut": false}))
    );

    // Anything but a number or "auto" is refused before reaching the engine.
    let r = call(&client, "camera_set", json!({"far": "infinite"})).await;
    assert_eq!(r.is_error, Some(true));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn terrain_status_reports_generation() {
    let engine = MockEngine::start(None, |method, _| match method {
        "terrain.status" => Action::Reply(json!({
            "present": true, "entity": 7, "streaming": true, "generating": true,
            "tiles_loaded": 4, "tiles_wanted": 9, "anchor": [0, 0, 0], "tile_size": 3000,
            "bounds": null})),
        _ => Action::Error(-32601, "unknown".into()),
    })
    .await;
    let client = connect(&engine.addr_string()).await;
    let r = call(&client, "terrain_status", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let v: Value = serde_json::from_str(&text(&r)).unwrap();
    assert_eq!(v["generating"], true);
    assert_eq!(v["tiles_wanted"], 9);
    client.cancel().await.unwrap();
}
