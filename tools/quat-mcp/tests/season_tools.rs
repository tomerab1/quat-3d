//! The season tools (`season_get`, `season_set`), end to end against the
//! mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

fn season() -> Value {
    json!({"in_scene": true, "entity": 7, "enabled": true, "mode": "independent",
           "year_phase": 0.875, "year": 2026, "phase_rate": 0.000_416_667,
           "year_minutes": 40.0, "day_minutes": 12.0, "season": "winter",
           "weights": {"spring": 0.0, "summer": 0.0, "autumn": 0.0, "winter": 1.0}})
}

/// A mock engine implementing season.get / season.set (docs/remote-control.md).
fn season_handler(method: &str, params: &Value) -> Action {
    match method {
        "season.get" => Action::Reply(season()),
        "season.set" => {
            if params.get("year_phase").and_then(Value::as_f64) > Some(1.0) {
                return Action::Error(-32602, "year_phase: expected a number in [0, 1]".into());
            }
            Action::Reply(season())
        }
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
async fn season_tools_are_listed_with_their_fields() {
    let engine = MockEngine::start(None, season_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in ["season_get", "season_set"] {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let set = tools.iter().find(|t| t.name == "season_set").unwrap();
    let schema = serde_json::to_string(&set.input_schema).unwrap();
    for field in [
        "mode",
        "season",
        "year_phase",
        "year_minutes",
        "day_minutes",
        "drive_clock_date",
        "real_calendar",
        "weather_preset",
        "climate",
        "default_climate",
        "spin_up_s",
    ] {
        assert!(schema.contains(field), "season_set lacks {field}");
    }
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn season_tools_forward_to_the_engine() {
    let engine = MockEngine::start(None, season_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "season_get", json!({"climate": true})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""season":"winter""#), "{}", text(&r));
    assert_eq!(
        engine.last_params("season.get"),
        Some(json!({"climate": true}))
    );

    let r = call(
        &client,
        "season_set",
        json!({"mode": "independent", "year_minutes": 40, "day_minutes": 12,
               "drive_clock_date": true, "weather_preset": "storm"}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("season.set"),
        Some(
            json!({"mode": "independent", "year_minutes": 40.0, "day_minutes": 12.0,
                    "drive_clock_date": true, "weather_preset": "storm"})
        )
    );

    // default_climate is the engine's `climate: null`.
    let r = call(&client, "season_set", json!({"default_climate": true})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("season.set"),
        Some(json!({"climate": null}))
    );

    let r = call(
        &client,
        "season_set",
        json!({"climate": {"temperature_c": [0, 10, 20, 10]}}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("season.set"),
        Some(json!({"climate": {"temperature_c": [0.0, 10.0, 20.0, 10.0]}}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn season_errors_reach_the_model() {
    let engine = MockEngine::start(None, season_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "season_set", json!({"year_phase": 1.5})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("year_phase: expected a number in [0, 1]"),
        "{}",
        text(&r)
    );

    // Contradictions, unknown fields and bad enum values never reach the engine.
    let before = engine.methods().len();
    let r = call(
        &client,
        "season_set",
        json!({"default_climate": true, "climate": {"cloud_cover": [0.5]}}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("not both"), "{}", text(&r));
    let r = call(
        &client,
        "season_set",
        json!({"season": "summer", "year_phase": 0.3}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(rejected(&client, "season_set", json!({"season": "monsoon"})).await);
    assert!(rejected(&client, "season_set", json!({"weather_preset": "hail"})).await);
    assert!(rejected(&client, "season_set", json!({"climate": {"fog": [1]}})).await);
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}
