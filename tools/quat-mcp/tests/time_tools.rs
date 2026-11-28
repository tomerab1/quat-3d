//! The world-clock tools (`time_*`) end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

fn state(datetime: &str) -> Value {
    json!({"in_scene": true, "enabled": true, "datetime": datetime, "utc_offset_h": 2.0,
           "latitude_deg": 48.8566, "longitude_deg": 2.3522, "time_scale": 1.0,
           "sun": {"azimuth_deg": 300.1, "elevation_deg": 4.2, "sunrise": "2026-06-21T05:46:57+02:00"},
           "moon": {"phase": "waxing crescent", "illuminated_fraction": 0.31}})
}

/// A mock engine implementing the world-clock methods.
fn time_handler(method: &str, params: &Value) -> Action {
    match method {
        "time.get" => Action::Reply(state("2026-06-21T12:00:00+02:00")),
        "time.set" => Action::Reply(state(
            params["datetime"]
                .as_str()
                .unwrap_or("2026-06-21T12:00:00+02:00"),
        )),
        "time.skip" if params["hours"].as_f64().is_some_and(|h| h.abs() > 87600.0) => {
            Action::Error(-32602, "hours: expected a number in [-87600, 87600]".into())
        }
        "time.skip" => Action::Reply(state("2026-06-21T15:00:00+02:00")),
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
async fn time_tools_are_listed_with_descriptions_and_schemas() {
    let engine = MockEngine::start(None, time_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in ["time_get", "time_set", "time_skip"] {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 120),
            "{name} needs an LLM-oriented description"
        );
    }
    let set = tools.iter().find(|t| t.name == "time_set").unwrap();
    let schema = serde_json::to_string(&set.input_schema).unwrap();
    for field in [
        "datetime",
        "local_time",
        "latitude_deg",
        "time_scale",
        "fixed_day",
    ] {
        assert!(schema.contains(field), "time_set schema lacks {field}");
    }
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn time_tools_forward_typed_params() {
    let engine = MockEngine::start(None, time_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "time_get", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("waxing crescent"), "{}", text(&r));

    let args = json!({"datetime": "2026-06-21T21:15", "latitude_deg": 48.86,
                      "longitude_deg": 2.35, "time_scale": 720.0, "fixed_day": true});
    let r = call(&client, "time_set", args.clone()).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    // Only the given fields reach the engine (omitted ones keep their values).
    assert_eq!(engine.last_params("time.set"), Some(args));

    let r = call(&client, "time_skip", json!({"hours": 3})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(engine.last_params("time.skip"), Some(json!({"hours": 3.0})));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn time_tool_errors_reach_the_model() {
    let engine = MockEngine::start(None, time_handler).await;
    let client = connect(&engine.addr_string()).await;

    // Engine-side validation: the precise message comes back as an error result.
    let r = call(&client, "time_skip", json!({"hours": 1e6})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("hours: expected a number"),
        "{}",
        text(&r)
    );

    // Unknown fields and missing `hours` are rejected before the engine.
    let before = engine.methods().len();
    for (tool, args) in [
        ("time_set", json!({"timezone": "CET"})),
        ("time_skip", json!({})),
    ] {
        let mut req = CallToolRequestParams::new(tool);
        req = req.with_arguments(args.as_object().unwrap().clone());
        let rejected = client
            .call_tool(req)
            .await
            .map_or(true, |r| r.is_error == Some(true));
        assert!(rejected, "{tool} {args}");
    }
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}
