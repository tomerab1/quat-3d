//! The physics-control tools (A10.1: `physics_*`, `events_poll`,
//! `character_move`), end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const PHYSICS_TOOLS: [&str; 10] = [
    "events_poll",
    "physics_get",
    "physics_set",
    "physics_apply",
    "physics_get_gravity",
    "physics_set_gravity",
    "physics_raycast_all",
    "physics_overlap",
    "physics_shape_cast",
    "character_move",
];

fn body_state() -> Value {
    json!({"entity": 7, "name": "Crate", "kind": "body", "motion_type": "dynamic",
           "layer": "dynamic", "sleeping": false, "mass": 2.0,
           "linear_velocity": [0, 2, 0], "angular_velocity": [0, 0, 0]})
}

/// A mock engine implementing the physics methods (docs/remote-control.md).
fn physics_handler(method: &str, params: &Value) -> Action {
    match method {
        "events.poll" => Action::Reply(json!({
            "events": [{"seq": 3, "kind": "contact_added", "time_s": 1.5, "entity_a": 7,
                        "entity_b": null, "point": [0, 0, 0], "normal": [0, 1, 0],
                        "impulse": 5.4}],
            "cursor": 3, "more": false, "dropped": 0, "oldest_seq": 1, "last_seq": 3,
            "capacity": 8192})),
        "physics.get" | "physics.set" | "physics.apply" => Action::Reply(body_state()),
        "physics.get_gravity" => {
            Action::Reply(json!({"gravity": [0, -9.81, 0], "magnitude": 9.81}))
        }
        "physics.set_gravity" => {
            Action::Reply(json!({"gravity": params["gravity"], "magnitude": 1.62}))
        }
        "physics.raycast_all" | "physics.overlap" | "physics.shape_cast" => Action::Reply(
            json!({"hits": [{"entity": 7, "name": "Crate", "kind": "body"}],
                                 "count": 1}),
        ),
        "character.move" => Action::Error(
            -32602,
            "velocity: horizontal speed 9 exceeds the controller's speed 4".into(),
        ),
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

fn text(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect::<Vec<_>>()
        .join("\n")
}

#[tokio::test]
async fn physics_tools_are_listed_with_annotations() {
    let engine = MockEngine::start(None, physics_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in PHYSICS_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
        assert!(tool.annotations.is_some(), "{name} needs annotations");
    }
    let hint = |name: &str| {
        tools
            .iter()
            .find(|t| t.name == name)
            .and_then(|t| t.annotations.clone())
            .unwrap()
    };
    for read_only in [
        "events_poll",
        "physics_get",
        "physics_get_gravity",
        "physics_raycast_all",
        "physics_overlap",
        "physics_shape_cast",
    ] {
        assert_eq!(hint(read_only).read_only_hint, Some(true), "{read_only}");
    }
    assert_eq!(hint("physics_set").idempotent_hint, Some(true));
    assert_eq!(hint("physics_apply").idempotent_hint, Some(false));
    assert_eq!(hint("physics_apply").destructive_hint, Some(false));
    assert_eq!(hint("character_move").destructive_hint, Some(false));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn tools_forward_their_parameters_to_the_engine() {
    let engine = MockEngine::start(None, physics_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "events_poll",
        json!({"since": 2, "kinds": ["contact_added"]}),
    )
    .await;
    assert_ne!(r.is_error, Some(true), "{}", text(&r));
    assert!(text(&r).contains("\"impulse\":5.4"));
    assert_eq!(
        engine.last_params("events.poll"),
        Some(json!({"since": 2, "kinds": ["contact_added"]}))
    );

    let r = call(
        &client,
        "physics_apply",
        json!({"entity": "Crate", "impulse": [0, 4, 0], "at_point": [0, 1, 0]}),
    )
    .await;
    assert_ne!(r.is_error, Some(true), "{}", text(&r));
    assert_eq!(
        engine.last_params("physics.apply"),
        Some(json!({"entity": "Crate", "impulse": [0.0, 4.0, 0.0], "at_point": [0.0, 1.0, 0.0]}))
    );

    call(
        &client,
        "physics_set",
        json!({"entity": 7, "motion_type": "kinematic", "friction": 0.5}),
    )
    .await;
    assert_eq!(
        engine.last_params("physics.set"),
        Some(json!({"entity": 7, "motion_type": "kinematic", "friction": 0.5}))
    );

    let r = call(
        &client,
        "physics_set_gravity",
        json!({"gravity": [0, -1.62, 0]}),
    )
    .await;
    assert!(text(&r).contains("1.62"));

    call(
        &client,
        "physics_overlap",
        json!({"shape": "box", "half_extents": [1, 1, 1], "position": [0, 2, 0],
               "layers": ["dynamic"]}),
    )
    .await;
    assert_eq!(
        engine.last_params("physics.overlap"),
        Some(json!({"shape": "box", "half_extents": [1.0, 1.0, 1.0],
                    "position": [0.0, 2.0, 0.0], "layers": ["dynamic"]}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn bad_input_is_refused_before_reaching_the_engine() {
    let engine = MockEngine::start(None, physics_handler).await;
    let client = connect(&engine.addr_string()).await;
    let cases = [
        ("physics_apply", json!({"entity": 7})),
        (
            "physics_apply",
            json!({"entity": 7, "impulse": [0, 1, 0], "torque": [0, 1, 0]}),
        ),
        ("physics_set", json!({"entity": 7})),
        (
            "physics_raycast_all",
            json!({"origin": [0, 0, 0], "direction": [0, 0, 0]}),
        ),
        (
            "physics_shape_cast",
            json!({"shape": "sphere", "from": [1, 1, 1], "to": [1, 1, 1]}),
        ),
        (
            "character_move",
            json!({"entity": 1, "velocity": [1, 0, 0], "target": [5, 0, 5]}),
        ),
        ("character_move", json!({"entity": 1})),
    ];
    for (tool, args) in cases {
        let r = call(&client, tool, args.clone()).await;
        assert_eq!(r.is_error, Some(true), "{tool} {args} should fail");
    }
    assert!(
        engine.methods().iter().all(|m| m == "session.hello"),
        "{:?}",
        engine.methods()
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn engine_errors_come_back_readable() {
    let engine = MockEngine::start(None, physics_handler).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(
        &client,
        "character_move",
        json!({"entity": "Hero", "velocity": [9, 0, 0], "jump": true}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("exceeds the controller's speed"),
        "{}",
        text(&r)
    );
    assert_eq!(
        engine.last_params("character.move"),
        Some(json!({"entity": "Hero", "velocity": [9.0, 0.0, 0.0], "jump": true}))
    );
    client.cancel().await.unwrap();
}
