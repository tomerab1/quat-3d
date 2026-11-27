//! The look-tuning tools (`render_settings_*`, `material_*`) and the
//! `tune_look` prompt, end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult, ContentBlock, GetPromptRequestParams};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const TUNING_TOOLS: [&str; 6] = [
    "render_settings_get",
    "render_settings_set",
    "render_settings_reset",
    "render_settings_schema",
    "material_get",
    "material_set",
];

fn settings() -> Value {
    json!({"exposure": {"auto": true, "compensation_ev": 0.0, "key": 0.18},
           "bloom": {"enabled": true, "intensity": 0.06},
           "taa": {"enabled": true, "feedback": 0.9}})
}

/// A mock engine implementing the A8 methods (docs/remote-control.md).
fn tuning_handler(method: &str, params: &Value) -> Action {
    match method {
        "render.settings.get" => match params.get("group").and_then(Value::as_str) {
            Some(g) => Action::Reply(json!({g: settings()[g]})),
            None => Action::Reply(settings()),
        },
        "render.settings.set" => {
            if params.pointer("/bloom/intensity").and_then(Value::as_f64) > Some(1.0) {
                return Action::Error(
                    -32602,
                    "bloom.intensity: expected a number in [0, 1]".into(),
                );
            }
            Action::Reply(settings())
        }
        "render.settings.reset" => Action::Reply(settings()),
        "render.settings.schema" => Action::Reply(json!({
            "groups": [{"name": "exposure", "label": "Exposure", "hint": "h",
                        "fields": [{"name": "compensation_ev", "type": "number", "min": -10,
                                    "max": 10, "default": 0, "unit": "EV", "hint": "h"}]}],
            "material": [{"name": "roughness", "kind": "scalar", "min": 0, "max": 1}]
        })),
        "material.get" | "material.set" => Action::Reply(json!({
            "id": 7, "name": "Car", "source": "mesh",
            "material": {"roughness": 0.2, "clearcoat": 1.0},
            "overridden": ["roughness"], "textures": [], "transparent": false, "shared_by": 2
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

/// Whether the call was refused (as a protocol error or an error result,
/// depending on the rmcp version) — used for inputs the schema rejects.
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
async fn tuning_tools_and_prompt_are_listed() {
    let engine = MockEngine::start(None, tuning_handler).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in TUNING_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 80),
            "{name} needs an LLM-oriented description"
        );
    }
    let set = tools
        .iter()
        .find(|t| t.name == "render_settings_set")
        .unwrap();
    let schema = serde_json::to_string(&set.input_schema).unwrap();
    for group in [
        "exposure", "bloom", "taa", "upscaler", "shadows", "ibl", "clouds",
    ] {
        assert!(schema.contains(group), "render_settings_set lacks {group}");
    }
    assert!(schema.contains("compensation_ev") && schema.contains("pcf_radius"));
    assert!(schema.contains("performance") && schema.contains("render_scale"));
    let material = tools.iter().find(|t| t.name == "material_set").unwrap();
    let schema = serde_json::to_string(&material.input_schema).unwrap();
    assert!(schema.contains("clearcoat_roughness") && schema.contains("revert"));

    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "tune_look"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn settings_tools_forward_to_the_engine() {
    let engine = MockEngine::start(None, tuning_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "render_settings_get", json!({"group": "taa"})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""feedback":0.9"#), "{}", text(&r));
    assert_eq!(
        engine.last_params("render.settings.get"),
        Some(json!({"group": "taa"}))
    );
    let r = call(&client, "render_settings_get", json!({})).await;
    assert!(text(&r).contains("exposure"));

    let r = call(
        &client,
        "render_settings_set",
        json!({"exposure": {"auto": false, "compensation_ev": 0.5}, "shadows": {"pcf_radius": 2}}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("render.settings.set"),
        Some(json!({"exposure": {"auto": false, "compensation_ev": 0.5},
                    "shadows": {"pcf_radius": 2}}))
    );

    let r = call(&client, "render_settings_reset", json!({"group": "bloom"})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("render.settings.reset"),
        Some(json!({"group": "bloom"}))
    );
    let r = call(&client, "render_settings_schema", json!({})).await;
    assert!(text(&r).contains(r#""unit":"EV""#), "{}", text(&r));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn settings_errors_reach_the_model() {
    let engine = MockEngine::start(None, tuning_handler).await;
    let client = connect(&engine.addr_string()).await;

    // Range errors come from the engine, with the field and a hint.
    let r = call(
        &client,
        "render_settings_set",
        json!({"bloom": {"intensity": 4}}),
    )
    .await;
    assert_eq!(r.is_error, Some(true));
    let out = text(&r);
    assert!(
        out.contains("bloom.intensity: expected a number in [0, 1]") && out.contains("Hint:"),
        "{out}"
    );

    // Unknown groups, fields and group names never reach the engine.
    let before = engine.methods().len();
    assert!(
        rejected(
            &client,
            "render_settings_set",
            json!({"fog": {"density": 1}})
        )
        .await
    );
    assert!(
        rejected(
            &client,
            "render_settings_set",
            json!({"bloom": {"glow": 1}})
        )
        .await
    );
    assert!(rejected(&client, "render_settings_get", json!({"group": "sky"})).await);
    let r = call(&client, "render_settings_set", json!({})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("nothing to change"));
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn material_tools_send_values_and_reverts() {
    let engine = MockEngine::start(None, tuning_handler).await;
    let client = connect(&engine.addr_string()).await;

    let r = call(&client, "material_get", json!({"entity": "Car"})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains(r#""shared_by":2"#));
    assert_eq!(
        engine.last_params("material.get"),
        Some(json!({"entity": "Car"}))
    );

    let r = call(
        &client,
        "material_set",
        json!({"entity": 7, "material": {"roughness": 0.2, "base_color": "#ff0000"},
               "revert": ["clearcoat"]}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert_eq!(
        engine.last_params("material.set"),
        Some(
            json!({"entity": 7, "material": {"roughness": 0.2, "base_color": "#ff0000",
                                              "clearcoat": null}})
        )
    );

    let before = engine.methods().len();
    let r = call(&client, "material_set", json!({"entity": 7})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(text(&r).contains("nothing to change"));
    assert!(
        rejected(
            &client,
            "material_set",
            json!({"entity": 7, "material": {"gloss": 1}})
        )
        .await
    );
    assert!(
        rejected(
            &client,
            "material_set",
            json!({"entity": 7, "revert": ["shininess"]})
        )
        .await
    );
    assert_eq!(engine.methods().len(), before);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn tune_look_prompt_puts_exposure_first() {
    let engine = MockEngine::start(None, tuning_handler).await;
    let client = connect(&engine.addr_string()).await;
    let args = json!({"goal": "warmer and less washed out", "entity": "Car"});
    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("tune_look")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(t.text.contains("The goal: warmer and less washed out"));
    assert!(t.text.contains("Material to focus on: Car"));
    // Workflow: screenshot -> read settings + schema -> exposure -> bloom ->
    // materials -> compare.
    let at = |s: &str| t.text.find(s).unwrap_or_else(|| panic!("missing {s}"));
    assert!(at("screenshot") < at("render_settings_get"));
    assert!(at("render_settings_get") < at("render_settings_schema"));
    assert!(at("Exposure first") < at("bloom"));
    assert!(at("bloom") < at("material_set"));
    assert!(at("material_set") < at("One change per step"));

    let bare = client
        .get_prompt(GetPromptRequestParams::new("tune_look"))
        .await
        .unwrap();
    let ContentBlock::Text(b) = &bare.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(!b.text.contains("The goal:") && !b.text.contains("{%"));
    client.cancel().await.unwrap();
}
