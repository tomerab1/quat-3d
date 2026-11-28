//! The runtime geometry tools (`geometry_*`, `mesh_dynamic_*`) end to end
//! against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

fn info(id: &str) -> Value {
    json!({"id": id, "version": 1, "resident_version": 0, "resident": false,
           "vertices": 14, "triangles": 6, "bytes": 696,
           "bounds": {"min": [0.0, 3.0, 0.0], "max": [8.0, 5.0, 5.0]}, "hash": "0a372be07661a58f"})
}

/// A mock engine implementing the geometry methods.
fn geometry_handler(method: &str, params: &Value) -> Action {
    match method {
        "geometry.generate"
            if params["spec"]["type"] == "roof"
                && params["spec"]["polygon"]
                    .as_array()
                    .is_some_and(|p| p.len() < 3) =>
        {
            Action::Error(
                -32602,
                "spec.polygon: ring 0 has 2 points, needs at least 3".into(),
            )
        }
        "geometry.generate" => Action::Reply(info(params["id"].as_str().unwrap_or("measured"))),
        "geometry.polygon" => Action::Reply(json!({"polygons": [], "area": 0.0})),
        "mesh.dynamic.list" => {
            Action::Reply(json!({"meshes": [info("a")], "stats": {"meshes": 1}}))
        }
        "mesh.dynamic.get" | "mesh.dynamic.set" | "mesh.dynamic.update_vertices" => {
            Action::Reply(info(params["id"].as_str().unwrap_or("?")))
        }
        "mesh.dynamic.remove" => Action::Reply(json!({"removed": true})),
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
async fn geometry_tools_are_listed_with_descriptions_and_schemas() {
    let engine = MockEngine::start(None, geometry_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in [
        "geometry_generate",
        "geometry_polygon",
        "mesh_dynamic_list",
        "mesh_dynamic_get",
        "mesh_dynamic_set",
        "mesh_dynamic_update_vertices",
        "mesh_dynamic_remove",
    ] {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 120),
            "{name} needs an LLM-oriented description"
        );
    }
    let generate = tools
        .iter()
        .find(|t| t.name == "geometry_generate")
        .unwrap();
    let schema = serde_json::to_string(&generate.input_schema).unwrap();
    for field in [
        "roof",
        "sweep",
        "extrude",
        "stone",
        "weights",
        "profile",
        "height_scale",
    ] {
        assert!(
            schema.contains(field),
            "geometry_generate schema lacks {field}"
        );
    }
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn geometry_tools_forward_typed_params() {
    let engine = MockEngine::start(None, geometry_handler).await;
    let client = connect(&engine.addr_string()).await;

    let args = json!({"spec": {"type": "roof", "polygon": [[0.0, 0.0], [8.0, 0.0], [8.0, 5.0], [0.0, 5.0]],
                               "weights": [1.0, 0.0, 1.0, 0.0], "base_height": 3.0},
                      "id": "house_roof"});
    let r = call(&client, "geometry_generate", args.clone()).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    // The tagged spec reaches the engine as written (omitted fields stay absent).
    assert_eq!(engine.last_params("geometry.generate"), Some(args));

    let args = json!({"op": "offset", "subject": [{"outer": [[0.0, 0.0], [4.0, 0.0], [4.0, 4.0]]}], "delta": -0.5});
    let r = call(&client, "geometry_polygon", args.clone()).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(engine.last_params("geometry.polygon"), Some(args));

    let args = json!({"id": "tri", "positions": [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], "indices": [0, 1, 2]});
    let r = call(&client, "mesh_dynamic_set", args.clone()).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(engine.last_params("mesh.dynamic.set"), Some(args));

    let r = call(&client, "mesh_dynamic_list", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let r = call(&client, "mesh_dynamic_remove", json!({"id": "tri"})).await;
    assert!(text(&r).contains("removed"), "{}", text(&r));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn geometry_tool_errors_reach_the_model() {
    let engine = MockEngine::start(None, geometry_handler).await;
    let client = connect(&engine.addr_string()).await;

    // Engine-side validation comes back as an error result naming the field.
    let r = call(
        &client,
        "geometry_generate",
        json!({"spec": {"type": "roof", "polygon": [[0.0, 0.0], [1.0, 0.0]]}}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("spec.polygon"), "{}", text(&r));

    // An unknown generator and unknown fields are rejected before the engine.
    let before = engine.methods().len();
    for (tool, args) in [
        ("geometry_generate", json!({"spec": {"type": "castle"}})),
        (
            "geometry_generate",
            json!({"spec": {"type": "stone", "colour": "red"}}),
        ),
        (
            "mesh_dynamic_set",
            json!({"id": "x", "positions": [], "indices": [], "colors": []}),
        ),
    ] {
        let mut req = CallToolRequestParams::new(tool);
        req = req.with_arguments(args.as_object().unwrap().clone());
        let rejected = client.call_tool(req).await;
        assert!(
            rejected.is_err() || rejected.as_ref().is_ok_and(|r| r.is_error == Some(true)),
            "{tool} accepted {args}"
        );
    }
    assert_eq!(
        engine.methods().len(),
        before,
        "invalid calls must not reach the engine"
    );
    client.cancel().await.unwrap();
}
