//! End to end: an rmcp client talks to `QuatServer` over an in-process duplex
//! pipe, and `QuatServer` talks to the mock engine over TCP.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{MockEngine, scene_handler};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{
    CallToolRequestParams, CallToolResult, ContentBlock, GetPromptRequestParams,
    ReadResourceRequestParams, ResourceContents,
};
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

#[tokio::test]
async fn lists_tools_prompts_and_resources() {
    let engine = MockEngine::start(None, scene_handler()).await;
    let client = connect(&engine.addr_string()).await;

    let tools = client.list_all_tools().await.unwrap();
    let names: Vec<&str> = tools.iter().map(|t| t.name.as_ref()).collect();
    for expected in [
        "engine_info",
        "engine_methods",
        "engine_call",
        "scene_components",
        "scene_list",
        "scene_get",
        "scene_create",
        "scene_update",
        "scene_remove_components",
        "scene_destroy",
        "scene_set_parent",
        "scene_clear",
        "scene_batch",
        "environment_get",
        "environment_set",
        "world_ground_height",
        "camera_get",
        "camera_set",
        "camera_frame",
        "screenshot",
        "asset_list_models",
        "asset_spawn_model",
        "sim_play",
        "sim_stop",
        "scene_save",
        "scene_load",
        "nav_bake",
    ] {
        assert!(
            names.contains(&expected),
            "missing tool {expected}: {names:?}"
        );
    }
    let create = tools.iter().find(|t| t.name == "scene_create").unwrap();
    let desc = create.description.as_deref().unwrap();
    assert!(desc.contains("Euler degrees") && desc.contains("YXZ") && desc.contains("#rrggbb"));
    let schema = serde_json::to_string(&create.input_schema).unwrap();
    assert!(schema.contains("components") && schema.contains("parent"));

    let prompts = client.list_all_prompts().await.unwrap();
    let prompt_names: Vec<&str> = prompts.iter().map(|p| p.name.as_str()).collect();
    assert!(prompt_names.contains(&"build_world"), "{prompt_names:?}");
    assert!(prompt_names.contains(&"inspect_scene"), "{prompt_names:?}");

    let resources = client.list_all_resources().await.unwrap();
    assert!(resources.iter().any(|r| r.uri == "quat://scene/summary"));

    // Listing touches nothing on the engine side.
    assert_eq!(engine.connections(), 0);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn scene_create_passes_components_through() {
    let engine = MockEngine::start(None, scene_handler()).await;
    let client = connect(&engine.addr_string()).await;

    let components = json!({
        "transform": {"position": [0, 0.5, 0], "rotation": [0, 45, 0], "scale": 2},
        "primitive": {"shape": "cube", "color": "#c0392b"}
    });
    let r = call(
        &client,
        "scene_create",
        json!({"name": "Crate", "parent": "Yard", "components": components}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let reply: Value = serde_json::from_str(&text(&r)).unwrap();
    assert_eq!(reply, json!({"id": 1, "name": "Crate"}));
    assert_eq!(
        engine.last_params("scene.create"),
        Some(json!({"name": "Crate", "parent": "Yard", "components": components}))
    );

    // A stringified components object is accepted too.
    let r = call(
        &client,
        "scene_create",
        json!({"components": "{\"primitive\":{\"shape\":\"sphere\"}}"}),
    )
    .await;
    assert_eq!(r.is_error, Some(false));
    assert_eq!(
        engine.last_params("scene.create"),
        Some(json!({"components": {"primitive": {"shape": "sphere"}}}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn engine_errors_become_readable_tool_errors() {
    let engine = MockEngine::start(None, scene_handler()).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "scene_update",
        json!({"entity": 4, "components": {"point_light": {"radius": -1}}}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    let t = text(&r);
    assert!(
        t.contains("scene.update failed — engine error invalid_params (-32602): point_light.radius: expected a number >= 0"),
        "{t}"
    );
    assert!(t.contains("Hint:"), "{t}");

    let r = call(&client, "scene_get", json!({"entity": "Ghost"})).await;
    assert_eq!(r.is_error, Some(true));
    let t = text(&r);
    assert!(
        t.contains("not_found (-32003): entity \"Ghost\" not found"),
        "{t}"
    );
    assert!(t.contains("scene_list"), "{t}");

    // Bad tool arguments are rejected before reaching the engine, as a tool
    // error naming the unknown field and the known ones.
    let r = call(&client, "environment_set", json!({"sun": {"elevation": 5}})).await;
    assert_eq!(r.is_error, Some(true));
    let t = text(&r);
    assert!(
        t.contains("unknown field `elevation`") && t.contains("elevation_deg"),
        "{t}"
    );
    assert!(engine.last_params("environment.set").is_none());

    // Local validation.
    let r = call(&client, "world_ground_height", json!({"points": []})).await;
    assert_eq!(r.is_error, Some(true));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn screenshot_returns_image_content() {
    let engine = MockEngine::start(None, scene_handler()).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "screenshot", json!({"max_size": 512})).await;
    assert_eq!(r.is_error, Some(false));
    let ContentBlock::Image(image) = &r.content[0] else {
        panic!("first content block should be an image: {:?}", r.content);
    };
    assert_eq!(image.mime_type, "image/png");
    assert!(image.data.starts_with("iVBORw0KGgo"));
    assert!(text(&r).contains("2x1 px"));
    assert_eq!(
        engine.last_params("render.screenshot"),
        Some(json!({"max_size": 512}))
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn scene_batch_normalizes_methods_and_summarizes_failures() {
    let engine = MockEngine::start(None, scene_handler()).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(
        &client,
        "scene_batch",
        json!({"commands": [
            {"method": "scene_create", "params": {"name": "House"}},
            {"method": "scene.create", "params": {"name": "Roof", "parent": "@0"}},
            {"method": "scene.explode", "params": {"entity": "@1"}}
        ], "stop_on_error": false}),
    )
    .await;
    assert_eq!(r.is_error, Some(false));
    let sent = engine.last_params("scene.batch").unwrap();
    assert_eq!(sent["commands"][0]["method"], "scene.create");
    assert_eq!(sent["commands"][1]["params"]["parent"], "@0");
    assert_eq!(sent["stop_on_error"], false);
    let t = text(&r);
    assert!(t.starts_with("1 of 3 commands failed."), "{t}");
    assert!(t.contains("command 2: method_not_found"), "{t}");
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn prompts_and_resource_render() {
    let engine = MockEngine::start(None, scene_handler()).await;
    let client = connect(&engine.addr_string()).await;

    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("build_world").with_arguments(
                json!({"description": "a lighthouse on a rocky island"})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(t.text.contains("a lighthouse on a rocky island"));
    assert!(t.text.contains("scene_batch") && t.text.contains("screenshot"));

    let summary = client
        .read_resource(ReadResourceRequestParams::new("quat://scene/summary"))
        .await
        .unwrap();
    let ResourceContents::TextResourceContents { text, .. } = &summary.contents[0] else {
        panic!("text resource expected");
    };
    let v: Value = serde_json::from_str(text).unwrap();
    assert_eq!(v["engine"]["frame"], 812);
    assert_eq!(v["scene"]["entities"][0]["name"], "Sun");
    assert!(v["environment"].is_null(), "unavailable parts are null");
    assert_eq!(v["camera"]["fov_deg"], 60);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn engine_not_running_is_a_tool_error() {
    let probe = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = probe.local_addr().unwrap().to_string();
    drop(probe);
    let client = connect(&addr).await;

    let r = call(&client, "engine_info", json!({})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("cannot connect to the quat-3d engine"));

    // Bring the engine up: the next call connects.
    let _engine = MockEngine::start_on(&addr, None, scene_handler()).await;
    let r = call(&client, "engine_info", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("\"frame\":812"));
    client.cancel().await.unwrap();
}
