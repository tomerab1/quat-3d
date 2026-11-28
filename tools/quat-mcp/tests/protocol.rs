//! The MCP protocol surface (ROADMAP A11.1), on both revisions quat-mcp
//! serves: 2025-11-25 (`initialize`) and 2026-07-28 (`server/discover`,
//! MRTR). Every test runs once per revision, whatever
//! `QUAT_MCP_TEST_PROTOCOL` says.

mod common;

use std::sync::{Arc, Mutex};
use std::time::Duration;

use common::{Action, MockEngine, client_with};
use quat_mcp::server::{ENTITY_TEMPLATE, SCRIPT_TEMPLATE, SHADER_TEMPLATE};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{
    CallToolRequestParams, CallToolResponse, CallToolResult, ClientCapabilities, ClientConfig,
    ElicitRequestParams, ElicitResult, ElicitationAction, ProtocolVersion,
    ReadResourceRequestParams, ResourceContents,
};
use rmcp::service::{RequestContext, RunningService};
use rmcp::{ClientHandler, ErrorData, RoleClient, Service, ServiceExt};
use serde_json::{Value, json};

const VERSIONS: [ProtocolVersion; 2] =
    [ProtocolVersion::V_2025_11_25, ProtocolVersion::V_2026_07_28];

fn handler(method: &str, params: &Value) -> Action {
    match method {
        "engine.info" => Action::Reply(
            json!({"engine":"quat-3d","frame":9,"entities":2,"playing":false,"paused":false,"editor":true}),
        ),
        "scene.create" => Action::Reply(json!({"id": 42, "name": params["name"]})),
        "scene.list" => Action::Reply(json!({"total": 2, "entities": [
            {"id": 1, "name": "Tower", "parent": null, "components": ["transform"],
             "world_position": [0.0, 1.0, 2.0], "world_bounds": null},
            {"id": 2, "name": "Tower_Roof", "parent": 1, "components": ["transform"],
             "world_position": [0.0, 5.0, 2.0], "world_bounds": {"min":[0,0,0],"max":[1,1,1]}},
        ]})),
        "scene.get" => match &params["entity"] {
            v if v == &json!(1) || v == &json!("Tower") => {
                Action::Reply(json!({"id": 1, "name": "Tower", "components": {}}))
            }
            _ => Action::Error(-32004, "entity: no entity 'Nope'".into()),
        },
        "scene.clear" => Action::Reply(json!({"destroyed": 7})),
        "scene.load" => Action::Reply(json!({"path": params["path"]})),
        "script.list" => Action::Reply(json!({"running": false, "scripts": [
            {"path": "ai/patrol.lua", "bytes": 10, "entities": []},
            {"path": "coin.lua", "bytes": 5, "entities": [3]},
        ]})),
        "script.get" => Action::Reply(
            json!({"path": params["path"], "source": "return 1", "entities": [], "status": null}),
        ),
        "script.delete" => Action::Reply(
            json!({"path": params["path"], "deleted": true, "entities_still_using": 0}),
        ),
        "shader.list" => Action::Reply(json!({"can_compile": true, "shaders": [
            {"name": "standard", "builtin": true, "path": "", "state": "ok", "reloads": 0, "errors": []},
            {"name": "lava", "builtin": false, "path": "lava.slang", "state": "ok", "reloads": 1, "errors": []},
        ]})),
        "shader.get" => {
            Action::Reply(json!({"name": params["name"], "source": "import material;"}))
        }
        // A reply that breaks `physics_get_gravity`'s output schema.
        "physics.get_gravity" => Action::Reply(json!({"gravity": "down"})),
        _ => Action::Error(-32601, format!("no method {method}")),
    }
}

/// A client that can elicit: it answers every confirmation with `answer`
/// (`None` declines) and records the questions.
#[derive(Clone, Default)]
struct Asker {
    answer: Option<bool>,
    questions: Arc<Mutex<Vec<String>>>,
}

impl ClientHandler for Asker {
    fn get_info(&self) -> ClientConfig {
        ClientConfig::new(
            ClientCapabilities::builder().enable_elicitation().build(),
            rmcp::model::Implementation::new("asker", "0"),
        )
    }

    async fn create_elicitation(
        &self,
        request: ElicitRequestParams,
        _context: RequestContext<RoleClient>,
    ) -> Result<ElicitResult, ErrorData> {
        if let ElicitRequestParams::FormElicitationParams {
            message,
            requested_schema,
            ..
        } = &request
        {
            let schema = serde_json::to_value(requested_schema).unwrap_or_default();
            assert_eq!(schema["properties"]["confirm"]["type"], "boolean");
            self.questions.lock().unwrap().push(message.clone());
        }
        Ok(match self.answer {
            Some(b) => {
                ElicitResult::new(ElicitationAction::Accept).with_content(json!({"confirm": b}))
            }
            None => ElicitResult::new(ElicitationAction::Decline),
        })
    }
}

async fn connect<S: Service<RoleClient>>(
    engine: &MockEngine,
    client: S,
    version: &ProtocolVersion,
) -> RunningService<RoleClient, S> {
    let mut cfg = EngineConfig::new(engine.addr_string());
    cfg.timeout = Duration::from_secs(5);
    let server = QuatServer::new(Arc::new(EngineClient::new(cfg)), Duration::from_secs(10));
    let (server_io, client_io) = tokio::io::duplex(1 << 20);
    tokio::spawn(async move {
        let running = server.serve(server_io).await.expect("server starts");
        let _ = running.waiting().await;
    });
    let running = client_with(client, client_io, version.clone()).await;
    let negotiated = running
        .peer_info()
        .expect("server info")
        .protocol_version
        .clone();
    assert_eq!(&negotiated, version);
    running
}

fn request(tool: &str, args: Value) -> CallToolRequestParams {
    let mut req = CallToolRequestParams::new(tool.to_owned());
    if let Value::Object(map) = args {
        req = req.with_arguments(map);
    }
    req
}

async fn call<S: Service<RoleClient>>(
    client: &RunningService<RoleClient, S>,
    tool: &str,
    args: Value,
) -> CallToolResult {
    client
        .call_tool(request(tool, args))
        .await
        .expect("tools/call")
}

fn text(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect::<Vec<_>>()
        .join("\n")
}

fn count(engine: &MockEngine, method: &str) -> usize {
    engine.methods().iter().filter(|m| *m == method).count()
}

#[tokio::test]
async fn tools_carry_annotations_icons_schemas_and_hints() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        let client = connect(&engine, (), v).await;
        let info = client.peer_info().expect("info");
        let icons = info
            .server_info
            .as_ref()
            .and_then(|s| s.icons.as_ref())
            .expect("server icons");
        assert_eq!(icons.len(), 2, "{v}");
        assert!(info.capabilities.completions.is_some(), "{v}");

        let tools = client.list_all_tools().await.expect("tools/list");
        let get = |n: &str| tools.iter().find(|t| t.name == n).expect(n);
        let clear = get("scene_clear");
        let a = clear.annotations.as_ref().expect("annotations");
        assert_eq!(a.destructive_hint, Some(true), "{v}");
        assert_eq!(a.open_world_hint, Some(false), "{v}");
        assert!(
            clear.input_schema["properties"]["confirm"].is_object(),
            "{v}"
        );
        let update = get("scene_update")
            .annotations
            .clone()
            .expect("annotations");
        assert_eq!(
            update.destructive_hint,
            Some(false),
            "{v}: not destructive by default"
        );
        assert!(get("engine_info").output_schema.is_some(), "{v}");
        assert!(get("scene_update").output_schema.is_none(), "{v}");
        let meta = get("scene_list").meta.as_ref().expect("meta");
        assert_eq!(
            meta.get("anthropic/maxResultSizeChars"),
            Some(&json!(400_000)),
            "{v}"
        );
        assert_eq!(
            get("asset_search")
                .annotations
                .as_ref()
                .unwrap()
                .open_world_hint,
            Some(true)
        );
        assert!(
            tools
                .iter()
                .all(|t| t.icons.as_ref().is_some_and(|i| i.len() == 2))
        );
        client.cancel().await.unwrap();
    }
}

#[tokio::test]
async fn stable_tools_return_structured_content() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        let client = connect(&engine, (), v).await;
        let r = call(&client, "engine_info", json!({})).await;
        assert_eq!(r.structured_content.as_ref().unwrap()["frame"], 9, "{v}");
        assert!(text(&r).contains("\"frame\":9"), "{v}: text content stays");

        let r = call(&client, "scene_list", json!({})).await;
        assert_eq!(r.structured_content.as_ref().unwrap()["total"], 2, "{v}");

        // Tools without a schema return no structured content.
        let r = call(&client, "scene_get", json!({"entity": 1})).await;
        assert!(r.structured_content.is_none(), "{v}");

        // A reply that no longer matches the contract is a readable error.
        let r = call(&client, "physics_get_gravity", json!({})).await;
        assert_eq!(r.is_error, Some(true), "{v}");
        assert!(text(&r).contains("output schema"), "{v}: {}", text(&r));
        client.cancel().await.unwrap();
    }
}

#[tokio::test]
async fn created_things_come_with_resource_links() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        let client = connect(&engine, (), v).await;
        let r = call(&client, "scene_create", json!({"name": "Box"})).await;
        let link = r
            .content
            .iter()
            .find_map(|c| c.as_resource_link())
            .expect("resource link");
        assert_eq!(link.uri, "quat://entity/42", "{v}");
        let read = client
            .read_resource(ReadResourceRequestParams::new("quat://entity/1"))
            .await
            .expect("read the linked entity");
        let ResourceContents::TextResourceContents { text, .. } = &read.contents[0] else {
            panic!("text contents");
        };
        assert!(text.contains("Tower"), "{v}");
        client.cancel().await.unwrap();
    }
}

#[tokio::test]
async fn templates_read_and_complete() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        let client = connect(&engine, (), v).await;
        let templates = client
            .list_all_resource_templates()
            .await
            .expect("templates");
        let uris: Vec<_> = templates.iter().map(|t| t.uri_template.clone()).collect();
        assert_eq!(
            uris,
            [ENTITY_TEMPLATE, SCRIPT_TEMPLATE, SHADER_TEMPLATE],
            "{v}"
        );

        for (uri, want) in [
            ("quat://entity/Tower", "\"id\":1"),
            ("quat://script/ai/patrol.lua", "return 1"),
            ("quat://shader/lava", "import material;"),
        ] {
            let read = client
                .read_resource(ReadResourceRequestParams::new(uri))
                .await
                .unwrap_or_else(|e| panic!("{v} {uri}: {e}"));
            let ResourceContents::TextResourceContents { text, .. } = &read.contents[0] else {
                panic!("text contents");
            };
            assert!(text.contains(want), "{v} {uri}: {text}");
        }
        assert_eq!(
            engine.last_params("script.get"),
            Some(json!({"path": "ai/patrol.lua"}))
        );
        let missing = client
            .read_resource(ReadResourceRequestParams::new("quat://entity/Nope"))
            .await
            .expect_err("unknown entity");
        assert!(missing.to_string().contains("Nope"), "{v}: {missing}");

        let c = client
            .complete_resource_simple(ENTITY_TEMPLATE, "entity", "Tow")
            .await
            .expect("complete entity");
        assert_eq!(c, ["Tower", "Tower_Roof"], "{v}");
        let c = client
            .complete_resource_simple(SCRIPT_TEMPLATE, "path", "ai/")
            .await
            .expect("complete script");
        assert_eq!(c, ["ai/patrol.lua"], "{v}");
        let c = client
            .complete_resource_simple(SHADER_TEMPLATE, "name", "la")
            .await
            .expect("complete shader");
        assert_eq!(c, ["lava"], "{v}");
        let bad = client
            .complete_resource_simple(ENTITY_TEMPLATE, "colour", "")
            .await
            .expect_err("unknown argument");
        assert!(bad.to_string().contains("colour"), "{v}: {bad}");
        client.cancel().await.unwrap();
    }
}

#[tokio::test]
async fn destructive_tools_ask_through_elicitation() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        let asker = Asker {
            answer: Some(true),
            ..Asker::default()
        };
        let questions = Arc::clone(&asker.questions);
        let client = connect(&engine, asker, v).await;
        let before = count(&engine, "scene.clear");
        let r = call(&client, "scene_clear", json!({"keep_lights": false})).await;
        assert_ne!(r.is_error, Some(true), "{v}: {}", text(&r));
        assert_eq!(
            r.structured_content.as_ref().unwrap()["destroyed"],
            7,
            "{v}"
        );
        assert_eq!(count(&engine, "scene.clear"), before + 1, "{v}");
        // The engine never sees `confirm`.
        assert_eq!(
            engine.last_params("scene.clear"),
            Some(json!({"keep_lights": false}))
        );
        let seen = questions.lock().unwrap().clone();
        assert_eq!(seen.len(), 1, "{v}");
        assert!(seen[0].contains("lights included"), "{v}: {}", seen[0]);
        client.cancel().await.unwrap();
    }
}

#[tokio::test]
async fn a_declined_or_refused_confirmation_changes_nothing() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        for answer in [None, Some(false)] {
            let client = connect(
                &engine,
                Asker {
                    answer,
                    ..Asker::default()
                },
                v,
            )
            .await;
            let r = call(&client, "script_delete", json!({"path": "coin.lua"})).await;
            assert_eq!(r.is_error, Some(true), "{v}");
            assert!(text(&r).contains("did not confirm"), "{v}: {}", text(&r));
            assert_eq!(count(&engine, "script.delete"), 0, "{v}");
            client.cancel().await.unwrap();
        }
    }
}

#[tokio::test]
async fn clients_without_elicitation_confirm_with_the_argument() {
    let engine = MockEngine::start(None, handler).await;
    for v in &VERSIONS {
        let client = connect(&engine, (), v).await;
        let before = count(&engine, "scene.load");
        let r = call(&client, "scene_load", json!({"path": "levels/a.json"})).await;
        assert_eq!(r.is_error, Some(true), "{v}");
        let t = text(&r);
        assert!(
            t.contains("confirm: true") && t.contains("levels/a.json"),
            "{v}: {t}"
        );
        assert_eq!(count(&engine, "scene.load"), before, "{v}");

        let r = call(
            &client,
            "scene_load",
            json!({"path": "levels/a.json", "confirm": "yes"}),
        )
        .await;
        assert!(text(&r).contains("must be true"), "{v}: {}", text(&r));

        let r = call(
            &client,
            "scene_load",
            json!({"path": "levels/a.json", "confirm": true}),
        )
        .await;
        assert_ne!(r.is_error, Some(true), "{v}: {}", text(&r));
        assert_eq!(
            engine.last_params("scene.load"),
            Some(json!({"path": "levels/a.json"}))
        );
        client.cancel().await.unwrap();
    }
}

#[tokio::test]
async fn mrtr_state_is_bound_to_the_call_and_used_once() {
    let engine = MockEngine::start(None, handler).await;
    let v = ProtocolVersion::V_2026_07_28;
    let client = connect(&engine, Asker::default(), &v).await;
    let args = json!({"name": "lava"});
    let first = client
        .call_tool_once(request("shader_delete", args.clone()))
        .await
        .expect("round 0");
    let CallToolResponse::InputRequired(input) = first else {
        panic!("expected input_required, got {first:?}");
    };
    let state = input.request_state.clone().expect("request state");
    assert!(
        input
            .input_requests
            .as_ref()
            .is_some_and(|r| r.contains_key("confirm"))
    );

    let yes = || {
        let mut r = rmcp::model::InputResponses::new();
        r.insert(
            "confirm".into(),
            json!({"action": "accept", "content": {"confirm": true}}),
        );
        r
    };
    // Other arguments with this state: refused, and the state is spent.
    let other = client
        .call_tool_once(
            request("shader_delete", json!({"name": "standard"}))
                .with_request_state(state.clone())
                .with_input_responses(yes()),
        )
        .await
        .expect("retry");
    let CallToolResponse::Complete(other) = other else {
        panic!("complete")
    };
    assert!(text(&other).contains("unknown, expired or for different arguments"));
    let again = client
        .call_tool_once(
            request("shader_delete", args.clone())
                .with_request_state(state)
                .with_input_responses(yes()),
        )
        .await
        .expect("retry");
    let CallToolResponse::Complete(again) = again else {
        panic!("complete")
    };
    assert_eq!(again.is_error, Some(true));
    assert_eq!(count(&engine, "shader.delete"), 0);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn legacy_peers_never_get_input_required() {
    // On 2025-11-25 the server asks with elicitation/create instead.
    let engine = MockEngine::start(None, handler).await;
    let asker = Asker {
        answer: Some(true),
        ..Asker::default()
    };
    let questions = Arc::clone(&asker.questions);
    let client = connect(&engine, asker, &ProtocolVersion::V_2025_11_25).await;
    let r = client
        .call_tool_once(request("shader_delete", json!({"name": "lava"})))
        .await
        .expect("call");
    assert!(matches!(r, CallToolResponse::Complete(_)), "{r:?}");
    assert_eq!(questions.lock().unwrap().len(), 1);
    client.cancel().await.unwrap();
}
