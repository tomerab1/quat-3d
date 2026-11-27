//! The behaviour-tree tools (`bt_*`) and the `design-npc-behaviour` prompt, end to
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

const BT_TOOLS: [&str; 8] = [
    "bt_leaves",
    "bt_get",
    "bt_set",
    "bt_add_node",
    "bt_remove_node",
    "bt_move_node",
    "bt_set_params",
    "bt_status",
];

/// The engine's tree view of a two-leaf selector (docs/remote-control.md).
fn view(extra: &Value) -> Value {
    let mut v = json!({
        "entity": 12, "enabled": true, "compiled": false, "parse_failed": false, "root": 1,
        "nodes": [
            {"id": 1, "kind": "selector", "children": [2, 3], "path": "root",
             "position": [125, 0]},
            {"id": 2, "kind": "leaf", "leaf": "near_camera", "params": {"range": 6.0},
             "children": [], "path": "root.children[0]", "position": [0, 190]},
            {"id": 3, "kind": "leaf", "leaf": "wait", "params": {"seconds": 0.5},
             "children": [], "path": "root.children[1]", "position": [250, 190]}
        ],
        "issues": [],
        "tree": {"type": "selector", "children": [
            {"type": "leaf", "name": "near_camera", "range": 6.0},
            {"type": "leaf", "name": "wait", "seconds": 0.5}]}
    });
    if let (Some(v), Some(extra)) = (v.as_object_mut(), extra.as_object()) {
        v.extend(extra.clone());
    }
    v
}

/// A mock engine implementing the bt.* methods.
fn bt_handler(method: &str, params: &Value) -> Action {
    match method {
        "bt.leaves" => Action::Reply(json!({
            "kinds": [{"name": "sequence", "min_children": 1, "max_children": null,
                       "help": "Runs children in order."}],
            "leaves": [{"name": "wait", "help": "Waits.", "params": [
                {"name": "seconds", "type": "number", "default": 1.0, "min": 0.0,
                 "max": 1_000_000.0, "optional": false, "help": "how long to wait"}]}]
        })),
        "bt.get" | "bt.move_node" | "bt.remove_node" | "bt.set_params" => {
            if params["entity"] == "Rock" {
                Action::Error(
                    -32003,
                    "entity: entity 7 has no behaviour tree; create one with bt.set".into(),
                )
            } else {
                Action::Reply(view(&json!({})))
            }
        }
        "bt.add_node" => Action::Reply(view(&json!({"node": 4}))),
        "bt.set" => {
            if params.pointer("/tree/children/1/seconds") == Some(&json!(-1.0)) {
                Action::ErrorWithData(
                    -32602,
                    "invalid behaviour tree, nothing was changed: node 3 at root.children[1]: \
                     wait.seconds: expected a number in [0, 1e+06]"
                        .into(),
                    json!({"issues": [{"node": 3, "path": "root.children[1]",
                        "message": "wait.seconds: expected a number in [0, 1e+06]"}]}),
                )
            } else {
                Action::Reply(view(&json!({"enabled": params["enabled"] != false})))
            }
        }
        "bt.status" => Action::Reply(json!({
            "entity": 12, "playing": true, "enabled": true, "compiled": true, "tick": 30,
            "root_status": "running",
            "nodes": [
                {"id": 1, "path": "root", "status": "running"},
                {"id": 2, "path": "root.children[0]", "status": "failure"},
                {"id": 3, "path": "root.children[1]", "status": "running"}]
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

fn text(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect::<Vec<_>>()
        .join("\n")
}

#[tokio::test]
async fn bt_tools_and_prompt_are_listed() {
    let engine = MockEngine::start(None, bt_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in BT_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let add = tools.iter().find(|t| t.name == "bt_add_node").unwrap();
    let schema = serde_json::to_string(&add.input_schema).unwrap();
    assert!(schema.contains("children") && schema.contains("index"));
    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "design-npc-behaviour"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn get_and_set_render_an_outline_with_node_ids() {
    let engine = MockEngine::start(None, bt_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "bt_get", json!({"entity": "Guard"})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let out = text(&r);
    assert!(
        out.contains("#1 selector\n  #2 near_camera range=6\n  #3 wait seconds=0.5\n"),
        "{out}"
    );
    assert!(out.contains("No issues."), "{out}");
    assert!(out.contains(r#"Tree JSON: {"root":1,"tree":{"#), "{out}");

    // A stringified tree is accepted and forwarded as an object.
    let tree = r#"{"type": "leaf", "name": "wait", "seconds": 2}"#;
    let r = call(
        &client,
        "bt_set",
        json!({"entity": 12, "tree": tree, "enabled": false}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("(disabled, "), "{}", text(&r));
    assert_eq!(
        engine.last_params("bt.set"),
        Some(json!({"entity": 12, "enabled": false,
                    "tree": {"type": "leaf", "name": "wait", "seconds": 2}}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn validation_errors_reach_the_model_with_their_node() {
    let engine = MockEngine::start(None, bt_handler).await;
    let client = connect(&engine.addr_string()).await;
    let bad = json!({"type": "selector", "children": [
        {"type": "leaf", "name": "near_camera"},
        {"type": "leaf", "name": "wait", "seconds": -1.0}]});
    let r = call(&client, "bt_set", json!({"entity": 12, "tree": bad})).await;
    assert_eq!(r.is_error, Some(true));
    let out = text(&r);
    assert!(out.contains("invalid_params"), "{out}");
    assert!(
        out.contains("node 3 at root.children[1]: wait.seconds: expected a number in [0, 1e+06]"),
        "{out}"
    );
    assert!(out.contains(r#"Details: {"issues":"#), "{out}");

    let r = call(&client, "bt_get", json!({"entity": "Rock"})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("create one with bt.set"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn edits_forward_typed_params() {
    let engine = MockEngine::start(None, bt_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "bt_add_node",
        json!({"entity": "Guard", "parent": 1, "index": 0, "kind": "inverter",
               "children": [2]}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).starts_with("Added node #4.\n"), "{}", text(&r));
    assert_eq!(
        engine.last_params("bt.add_node"),
        Some(
            json!({"entity": "Guard", "parent": 1, "index": 0, "kind": "inverter",
                    "children": [2]})
        )
    );
    // Without a parent the engine is asked for a new root (parent null).
    call(
        &client,
        "bt_add_node",
        json!({"entity": 12, "kind": "leaf", "leaf": "wait", "params": {"seconds": 2},
               "children": null}),
    )
    .await;
    assert_eq!(
        engine.last_params("bt.add_node"),
        Some(
            json!({"entity": 12, "parent": null, "kind": "leaf", "leaf": "wait",
                    "params": {"seconds": 2.0}})
        )
    );

    call(
        &client,
        "bt_move_node",
        json!({"entity": 12, "node": 3, "parent": 1, "index": 0}),
    )
    .await;
    assert_eq!(
        engine.last_params("bt.move_node"),
        Some(json!({"entity": 12, "node": 3, "parent": 1, "index": 0}))
    );
    call(
        &client,
        "bt_remove_node",
        json!({"entity": 12, "node": 2, "keep_children": true}),
    )
    .await;
    assert_eq!(
        engine.last_params("bt.remove_node"),
        Some(json!({"entity": 12, "node": 2, "keep_children": true}))
    );
    call(
        &client,
        "bt_set_params",
        json!({"entity": 12, "node": 3, "params": {"seconds": 4, "unused": null}}),
    )
    .await;
    assert_eq!(
        engine.last_params("bt.set_params"),
        Some(json!({"entity": 12, "node": 3, "params": {"seconds": 4.0, "unused": null}}))
    );

    // An empty parameter set is answered without a round trip.
    let before = engine.methods().len();
    let r = call(
        &client,
        "bt_set_params",
        json!({"entity": 12, "node": 3, "params": {}}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn status_names_the_nodes_and_leaves_pass_through() {
    let engine = MockEngine::start(None, bt_handler).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(&client, "bt_status", json!({"entity": 12})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let out = text(&r);
    assert!(
        out.starts_with("Entity 12: playing, tree enabled, tick 30, root running.\n"),
        "{out}"
    );
    assert!(
        out.contains("#1 selector: running\n  #2 near_camera range=6: failure\n"),
        "{out}"
    );
    assert!(out.contains("  #3 wait seconds=0.5: running"), "{out}");

    let r = call(&client, "bt_leaves", json!({})).await;
    assert!(text(&r).contains(r#""name":"wait""#), "{}", text(&r));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn design_npc_behaviour_prompt_walks_the_workflow() {
    let engine = MockEngine::start(None, bt_handler).await;
    let client = connect(&engine.addr_string()).await;
    let args = json!({"goal": "patrol the gate and chase the player when near", "entity": "Guard"});
    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("design-npc-behaviour")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(
        t.text
            .contains("The goal: patrol the gate and chase the player when near")
    );
    assert!(t.text.contains("Give the behaviour to this entity: Guard"));
    let at = |s: &str| t.text.find(s).unwrap_or_else(|| panic!("missing {s}"));
    assert!(at("bt_leaves") < at("bt_set"));
    assert!(at("bt_set") < at("sim_play"));
    assert!(at("sim_play") < at("bt_status"));
    assert!(at("bt_status") < at("sim_stop"));
    assert!(!t.text.contains("{%"));
    client.cancel().await.unwrap();
}
