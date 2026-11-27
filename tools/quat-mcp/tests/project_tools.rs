//! The game-packaging tools (`project_info`, `project_build`,
//! `project_build_status`), end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const PROJECT_TOOLS: [&str; 3] = ["project_info", "project_build", "project_build_status"];

/// A mock engine implementing the P4 methods (docs/remote-control.md).
fn project_handler(method: &str, params: &Value) -> Action {
    match method {
        "project.info" => Action::Reply(json!({
            "open": true, "root": "/games/coin-collector", "executable": "coin-collector",
            "host_target": "linux-x64",
            "game": {"format": 1, "name": "Coin Collector", "id": "dev.quat3d.coin-collector",
                     "version": "0.1.0", "start_scene": "assets/scenes/coin_collector.json"},
            "build": {"state": "idle", "job": 0}
        })),
        "project.build" => {
            if params.get("target").and_then(Value::as_str) == Some("windows-x64") {
                return Action::Error(-32004, "cannot package for windows-x64 on linux-x64".into());
            }
            Action::Reply(json!({"job": 1, "state": "running", "target": "linux-x64",
                                 "config": "shipping", "output": "dist"}))
        }
        "project.build_status" => Action::Reply(json!({
            "job": 1, "state": "succeeded", "progress": 1.0, "step": 4, "steps": 4,
            "step_name": "lay out linux-x64 package", "elapsed_s": 12.5,
            "package": "dist/coin-collector-0.1.0-linux-x64.tar.gz",
            "folder": "dist/coin-collector-0.1.0-linux-x64", "log": ["[pack] step 4/4"]
        })),
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

async fn rejected(client: &RunningService<RoleClient, ()>, tool: &str, args: Value) -> bool {
    let mut req = CallToolRequestParams::new(tool.to_owned());
    if let Value::Object(map) = args {
        req = req.with_arguments(map);
    }
    client
        .call_tool(req)
        .await
        .map_or(true, |r| r.is_error == Some(true))
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
async fn project_tools_are_listed_with_annotations() {
    let engine = MockEngine::start(None, project_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in PROJECT_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let read_only = |name: &str| {
        tools
            .iter()
            .find(|t| t.name == name)
            .and_then(|t| t.annotations.as_ref())
            .and_then(|a| a.read_only_hint)
    };
    assert_eq!(read_only("project_info"), Some(true));
    assert_eq!(read_only("project_build_status"), Some(true));
    assert_ne!(read_only("project_build"), Some(true));
    let build = tools.iter().find(|t| t.name == "project_build").unwrap();
    let schema = serde_json::to_string(&build.input_schema).unwrap();
    for field in [
        "target",
        "output",
        "config",
        "smoke_test",
        "macos-arm64",
        "development",
    ] {
        assert!(schema.contains(field), "project_build schema lacks {field}");
    }
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn project_tools_forward_to_the_engine() {
    let engine = MockEngine::start(None, project_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "project_info", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("coin-collector"), "{}", text(&r));

    let r = call(
        &client,
        "project_build",
        json!({"output": "builds", "config": "development", "smoke_test": true}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("project.build"),
        Some(json!({"output": "builds", "config": "development", "smoke_test": true}))
    );

    let r = call(&client, "project_build_status", json!({"log_lines": 10})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("coin-collector-0.1.0-linux-x64.tar.gz"));
    assert_eq!(
        engine.last_params("project.build_status"),
        Some(json!({"log_lines": 10}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn project_build_errors_reach_the_model() {
    let engine = MockEngine::start(None, project_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "project_build", json!({"target": "windows-x64"})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("cannot package for windows-x64"),
        "{}",
        text(&r)
    );

    // Bad arguments never reach the engine.
    let before = engine.methods().len();
    assert!(rejected(&client, "project_build", json!({"target": "amiga"})).await);
    assert!(rejected(&client, "project_build", json!({"config": "debug"})).await);
    assert!(rejected(&client, "project_build", json!({"outptu": "x"})).await);
    assert!(rejected(&client, "project_build_status", json!({"log_lines": 5000})).await);
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}
