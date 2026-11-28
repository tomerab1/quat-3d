//! The terrain edit tools (`terrain_edit_*`), end to end against the mock
//! engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const TOOLS: [&str; 5] = [
    "terrain_edit_add",
    "terrain_edit_update",
    "terrain_edit_remove",
    "terrain_edit_list",
    "terrain_edit_clear",
];

/// A mock engine implementing terrain.edit.* (docs/remote-control.md).
fn terrain_handler(method: &str, params: &Value) -> Action {
    match method {
        "terrain.edit.add" => {
            if params.get("radius").and_then(Value::as_f64) > Some(4096.0) {
                return Action::Error(
                    -32602,
                    "radius: expected a number in [0, 4096] (metres)".into(),
                );
            }
            Action::Reply(json!({"id": 1, "revision": 2, "count": 1}))
        }
        "terrain.edit.update" => Action::Reply(json!({"id": 1, "revision": 3, "count": 1})),
        "terrain.edit.remove" => {
            Action::Reply(json!({"removed": [1], "missing": [], "revision": 4, "count": 0}))
        }
        "terrain.edit.list" => Action::Reply(json!({"stamps": [{"id": 1, "shape": "circle",
            "points": [[0.0, 0.0]], "radius": 3.0, "falloff": 1.0, "op": "raise",
            "strength": 1.0, "amount": 2.0}], "revision": 2, "count": 1})),
        "terrain.edit.clear" => Action::Reply(json!({"removed": 1, "revision": 5, "count": 0})),
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
async fn terrain_edit_tools_are_listed() {
    let engine = MockEngine::start(None, terrain_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let find = |name: &str| tools.iter().find(|t| t.name == name).unwrap();
    let list = find("terrain_edit_list");
    assert_eq!(
        list.annotations.as_ref().and_then(|a| a.read_only_hint),
        Some(true)
    );
    for name in ["terrain_edit_remove", "terrain_edit_clear"] {
        assert_eq!(
            find(name)
                .annotations
                .as_ref()
                .and_then(|a| a.destructive_hint),
            Some(true),
            "{name} is destructive"
        );
    }
    let add = serde_json::to_string(&find("terrain_edit_add").input_schema).unwrap();
    for field in [
        "shape",
        "points",
        "radius",
        "falloff",
        "op",
        "strength",
        "height",
        "pivot",
        "normal",
        "amount",
        "iterations",
        "channel",
        "value",
        "before",
        "polygon",
        "flatten",
        "gravel",
    ] {
        assert!(add.contains(field), "terrain_edit_add lacks {field}");
    }
    let update = serde_json::to_string(&find("terrain_edit_update").input_schema).unwrap();
    for field in ["id", "shape", "points", "height", "channel"] {
        assert!(update.contains(field), "terrain_edit_update lacks {field}");
    }
    let remove = serde_json::to_string(&find("terrain_edit_remove").input_schema).unwrap();
    assert!(remove.contains("ids"), "terrain_edit_remove lacks ids");
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn terrain_edit_tools_forward_to_the_engine() {
    let engine = MockEngine::start(None, terrain_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "terrain_edit_add",
        json!({"shape": "polygon", "points": [[10, 4], [18, 4], [18, 10], [10, 10]],
               "radius": 1, "falloff": 2, "op": "flatten", "height": 22.5}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""id":1"#), "{}", text(&r));
    assert_eq!(
        engine.last_params("terrain.edit.add"),
        Some(json!({"shape": "polygon",
            "points": [[10.0, 4.0], [18.0, 4.0], [18.0, 10.0], [10.0, 10.0]],
            "radius": 1.0, "falloff": 2.0, "op": "flatten", "height": 22.5}))
    );

    let r = call(
        &client,
        "terrain_edit_add",
        json!({"shape": "capsule", "points": [[0, 0], [12, 3]], "radius": 1.2,
               "op": "paint", "channel": "grass", "value": 0, "before": 1}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("terrain.edit.add"),
        Some(
            json!({"shape": "capsule", "points": [[0.0, 0.0], [12.0, 3.0]],
            "radius": 1.2, "op": "paint", "channel": "grass", "value": 0.0, "before": 1})
        )
    );

    let r = call(
        &client,
        "terrain_edit_update",
        json!({"id": 1, "op": "smooth", "iterations": 3}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("terrain.edit.update"),
        Some(json!({"id": 1, "op": "smooth", "iterations": 3}))
    );

    let r = call(&client, "terrain_edit_remove", json!({"ids": [1, 9]})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("terrain.edit.remove"),
        Some(json!({"ids": [1, 9]}))
    );
    let r = call(&client, "terrain_edit_remove", json!({"id": 1})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("terrain.edit.remove"),
        Some(json!({"id": 1}))
    );

    let r = call(&client, "terrain_edit_list", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""op":"raise""#), "{}", text(&r));

    let r = call(&client, "terrain_edit_clear", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""removed":1"#), "{}", text(&r));

    let methods = engine.methods();
    for method in ["terrain.edit.list", "terrain.edit.clear"] {
        assert!(methods.iter().any(|m| m == method), "{method} not called");
    }
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn terrain_edit_errors_reach_the_model() {
    let engine = MockEngine::start(None, terrain_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "terrain_edit_add",
        json!({"shape": "circle", "points": [[0, 0]], "op": "raise", "radius": 5000}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("radius: expected a number in [0, 4096]"),
        "{}",
        text(&r)
    );

    // Requests the client can already tell are wrong never reach the engine.
    let before = engine.methods().len();
    let r = call(&client, "terrain_edit_update", json!({"id": 3})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("nothing to change"), "{}", text(&r));

    let r = call(&client, "terrain_edit_remove", json!({})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("`id`"), "{}", text(&r));
    let r = call(&client, "terrain_edit_remove", json!({"id": 1, "ids": [2]})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("not both"), "{}", text(&r));
    let r = call(&client, "terrain_edit_remove", json!({"ids": []})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("empty"), "{}", text(&r));

    let many: Vec<[f64; 2]> = (0..300).map(|i| [f64::from(i), 0.0]).collect();
    let r = call(
        &client,
        "terrain_edit_add",
        json!({"shape": "capsule", "points": many, "op": "raise", "amount": 1}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("300 points"), "{}", text(&r));
    let r = call(
        &client,
        "terrain_edit_update",
        json!({"id": 1, "points": many}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("at most 256"), "{}", text(&r));

    let stamp = json!({"shape": "circle", "points": [[0, 0]], "op": "raise"});
    let mut unknown = stamp.clone();
    unknown["depth"] = json!(2);
    assert!(rejected(&client, "terrain_edit_add", unknown).await);
    let mut bad_op = stamp.clone();
    bad_op["op"] = json!("dig");
    assert!(rejected(&client, "terrain_edit_add", bad_op).await);
    let mut bad_channel = stamp;
    bad_channel["channel"] = json!("sand");
    assert!(rejected(&client, "terrain_edit_add", bad_channel).await);
    assert!(rejected(&client, "terrain_edit_add", json!({"op": "raise"})).await);
    assert!(rejected(&client, "terrain_edit_update", json!({"height": 2})).await);
    assert!(
        rejected(
            &client,
            "terrain_edit_remove",
            json!({"id": 1, "all": true})
        )
        .await
    );
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}
