//! The weather tools (`weather_get`, `weather_set`) and the `set-weather`
//! prompt, end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult, GetPromptRequestParams};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

fn weather() -> Value {
    json!({"present": true, "entity": 12, "weather": {
        "wind_dir_deg": 35.0, "wind_speed": 4.0, "gustiness": 0.35, "turbulence": 0.3,
        "cloud_cover": 0.42, "precipitation": 0.0, "precipitation_type": "rain",
        "wetness": 0.0, "snow": 0.0, "temperature_c": 12.0, "accumulate": true}})
}

/// A mock engine implementing weather.get / weather.set (docs/remote-control.md).
fn weather_handler(method: &str, params: &Value) -> Action {
    match method {
        "weather.get" => Action::Reply(weather()),
        "weather.set" => {
            if params.get("wind_speed").and_then(Value::as_f64) > Some(60.0) {
                return Action::Error(
                    -32602,
                    "weather.wind_speed: expected a number in [0, 60] (m/s)".into(),
                );
            }
            Action::Reply(weather())
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
async fn weather_tools_and_prompt_are_listed() {
    let engine = MockEngine::start(None, weather_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in ["weather_get", "weather_set"] {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let set = tools.iter().find(|t| t.name == "weather_set").unwrap();
    let schema = serde_json::to_string(&set.input_schema).unwrap();
    for field in [
        "wind_speed",
        "gustiness",
        "cloud_cover",
        "precipitation_type",
        "wetness",
        "snow",
        "temperature_c",
        "accumulate",
    ] {
        assert!(schema.contains(field), "weather_set lacks {field}");
    }
    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "set-weather"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn weather_tools_forward_to_the_engine() {
    let engine = MockEngine::start(None, weather_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "weather_get", json!({"wind_at": [[1, 10, 2]]})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""wind_speed":4.0"#), "{}", text(&r));
    assert_eq!(
        engine.last_params("weather.get"),
        Some(json!({"wind_at": [[1.0, 10.0, 2.0]]}))
    );

    let r = call(
        &client,
        "weather_set",
        json!({"precipitation": 0.5, "precipitation_type": "snow", "temperature_c": -3}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("weather.set"),
        Some(json!({"precipitation": 0.5, "precipitation_type": "snow", "temperature_c": -3.0}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn weather_errors_reach_the_model() {
    let engine = MockEngine::start(None, weather_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "weather_set", json!({"wind_speed": 90})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("weather.wind_speed: expected a number in [0, 60]"),
        "{}",
        text(&r)
    );

    // Empty sets, unknown fields and bad enum values never reach the engine.
    let before = engine.methods().len();
    let r = call(&client, "weather_set", json!({})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("nothing to change"), "{}", text(&r));
    assert!(rejected(&client, "weather_set", json!({"fog": 1})).await);
    assert!(
        rejected(
            &client,
            "weather_set",
            json!({"precipitation_type": "hail"})
        )
        .await
    );
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn set_weather_prompt_renders_the_description() {
    let engine = MockEngine::start(None, weather_handler).await;
    let client = connect(&engine.addr_string()).await;
    let mut args = serde_json::Map::new();
    args.insert("description".into(), json!("  light snowfall at dusk "));
    let prompt = client
        .get_prompt(GetPromptRequestParams::new("set-weather").with_arguments(args))
        .await
        .unwrap();
    let body = serde_json::to_string(&prompt.messages).unwrap();
    assert!(
        body.contains("The weather to reach: light snowfall at dusk"),
        "{body}"
    );
    assert!(body.contains("weather_set"), "{body}");

    let bare = client
        .get_prompt(GetPromptRequestParams::new("set-weather"))
        .await
        .unwrap();
    let body = serde_json::to_string(&bare.messages).unwrap();
    assert!(!body.contains("The weather to reach"), "{body}");
    client.cancel().await.unwrap();
}
