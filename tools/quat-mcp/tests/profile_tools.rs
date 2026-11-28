//! The profiling tools (`profile_*`) and the `find-performance-problem`
//! prompt, end to end against the mock engine.

mod common;

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult, ContentBlock, GetPromptRequestParams};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};

const PROFILE_TOOLS: [&str; 3] = ["profile_frame", "profile_capture", "profile_counters"];

fn summary(id: u64) -> Value {
    json!({
        "id": id, "status": "done", "frames": 3, "first_frame": 50, "budget_ms": 16.667,
        "frame_ms": {"total": {"avg": 16.0, "p50": 15.0, "p95": 22.0, "max": 25.0}},
        "gpu_frames": 3,
        "top_by_total": [{"name": "lighting", "kind": "pass", "frames": 3,
                          "gpu": {"total": 6.0, "avg": 2.0, "p95": 2.5, "max": 2.5}}],
        "top_by_p95": [],
        "spikes": {"count": 1, "worst": [{"frame": 51, "total_ms": 25.0, "cpu_ms": 9.0,
                   "gpu_ms": 24.0, "zones": [{"name": "lighting", "ms": 2.5, "on": "gpu"}]}]},
        "per_frame": {"total_ms": [15.0, 25.0, 8.0], "gpu_ms": [14.0, 24.0, 7.0]}
    })
}

fn recording(id: u64, done: u64) -> Value {
    json!({"id": id, "status": "recording", "frames": 3, "frames_done": done,
           "waiting_for_gpu": done >= 3})
}

/// A mock engine whose captures finish after `polls_until_done` result polls
/// (`usize::MAX`: never).
fn profile_handler(
    polls_until_done: usize,
) -> impl Fn(&str, &Value) -> Action + Send + Sync + 'static {
    let polls = Arc::new(AtomicUsize::new(0));
    move |method, params| match method {
        "profile.frame" => Action::Reply(json!({
            "frame": 900, "window": 240, "frames": 240, "fps": 60.1,
            "frame_ms": {"total": {"avg": 16.6, "p50": 16.6, "p95": 17.0, "max": 20.0}},
            "gpu_timing": "ok", "zones": [], "zones_matched": 0, "counters": {"draws": 12}
        })),
        "profile.capture" => {
            if params.get("frames").and_then(Value::as_u64) == Some(1001) {
                return Action::Error(-32602, "frames: expected an integer in [1, 1000]".into());
            }
            Action::Reply(json!({"id": 4, "status": "recording", "frames": 3,
                                 "budget_ms": 16.667, "gpu_timing": true}))
        }
        "profile.result" => {
            let id = params["id"].as_u64().unwrap_or(0);
            if id != 4 {
                return Action::Error(-32003, format!("capture {id} not found"));
            }
            let n = polls.fetch_add(1, Ordering::SeqCst) + 1;
            if n >= polls_until_done {
                Action::Reply(summary(id))
            } else {
                Action::Reply(recording(id, n as u64))
            }
        }
        "profile.counters" => Action::Reply(json!({
            "frame": 900, "counters": {"draws": 12, "triangles": 3400},
            "gpu_memory": null, "entities": {"total": 5}
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

fn count(engine: &MockEngine, method: &str) -> usize {
    engine.methods().iter().filter(|m| *m == method).count()
}

#[tokio::test]
async fn profile_tools_and_prompt_are_listed() {
    let engine = MockEngine::start(None, profile_handler(1)).await;
    let client = connect(&engine.addr_string()).await;
    let tools = client.list_all_tools().await.unwrap();
    for name in PROFILE_TOOLS {
        let tool = tools
            .iter()
            .find(|t| t.name == name)
            .unwrap_or_else(|| panic!("missing tool {name}"));
        assert!(
            tool.description.as_deref().is_some_and(|d| d.len() > 200),
            "{name} needs an LLM-oriented description"
        );
    }
    let capture = tools.iter().find(|t| t.name == "profile_capture").unwrap();
    let schema = serde_json::to_string(&capture.input_schema).unwrap();
    assert!(schema.contains("budget_ms") && schema.contains("max_wait_s"));
    let prompts = client.list_all_prompts().await.unwrap();
    assert!(prompts.iter().any(|p| p.name == "find-performance-problem"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn profile_frame_forwards_sort_kind_and_limit() {
    let engine = MockEngine::start(None, profile_handler(1)).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(
        &client,
        "profile_frame",
        json!({"sort": "gpu", "kind": "pass", "limit": 5}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("\"fps\":60.1"));
    assert_eq!(
        engine.last_params("profile.frame"),
        Some(json!({"sort": "gpu", "kind": "pass", "limit": 5}))
    );
    let bare = call(&client, "profile_frame", json!({})).await;
    assert_eq!(bare.is_error, Some(false));
    assert_eq!(engine.last_params("profile.frame"), Some(json!({})));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn profile_capture_starts_then_polls_until_done() {
    let engine = MockEngine::start(None, profile_handler(3)).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(
        &client,
        "profile_capture",
        json!({"frames": 3, "budget_ms": 8.33, "max_wait_s": 10}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let out: Value = serde_json::from_str(&text(&r)).expect("the summary is JSON");
    assert_eq!(out["status"], "done");
    assert_eq!(out["spikes"]["count"], 1);
    // Only the capture settings reach the engine.
    assert_eq!(
        engine.last_params("profile.capture"),
        Some(json!({"frames": 3, "budget_ms": 8.33}))
    );
    assert_eq!(engine.last_params("profile.result"), Some(json!({"id": 4})));
    assert_eq!(count(&engine, "profile.capture"), 1);
    assert_eq!(count(&engine, "profile.result"), 3);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn profile_capture_with_an_id_only_polls() {
    let engine = MockEngine::start(None, profile_handler(1)).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(&client, "profile_capture", json!({"id": 4})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("\"status\":\"done\""));
    assert_eq!(count(&engine, "profile.capture"), 0);

    let unknown = call(&client, "profile_capture", json!({"id": 9})).await;
    assert_eq!(unknown.is_error, Some(true));
    assert!(text(&unknown).contains("capture 9 not found"));
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn profile_capture_reports_progress_when_the_wait_runs_out() {
    let engine = MockEngine::start(None, profile_handler(usize::MAX)).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(&client, "profile_capture", json!({"max_wait_s": 1})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let out = text(&r);
    assert!(out.starts_with("Capture 4 is still recording"), "{out}");
    assert!(out.contains(r#"{"id":4}"#), "{out}");
    // Bounded: about one poll per 250 ms within the second it waited.
    let polls = count(&engine, "profile.result");
    assert!((2..=6).contains(&polls), "{polls} polls");
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn profile_capture_rejects_conflicting_or_bad_inputs() {
    let engine = MockEngine::start(None, profile_handler(1)).await;
    let client = connect(&engine.addr_string()).await;
    let both = call(&client, "profile_capture", json!({"id": 4, "frames": 10})).await;
    assert_eq!(both.is_error, Some(true));
    assert!(text(&both).contains("not both"));
    let wait = call(&client, "profile_capture", json!({"max_wait_s": 600})).await;
    assert_eq!(wait.is_error, Some(true));
    assert!(text(&wait).contains("max_wait_s"));
    assert!(engine.methods().iter().all(|m| !m.starts_with("profile.")));

    // Engine-side validation errors come back with their message and hint.
    let frames = call(&client, "profile_capture", json!({"frames": 1001})).await;
    assert_eq!(frames.is_error, Some(true));
    assert!(text(&frames).contains("frames: expected an integer in [1, 1000]"));
    assert_eq!(count(&engine, "profile.result"), 0);
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn profile_counters_calls_the_engine_without_params() {
    let engine = MockEngine::start(None, profile_handler(1)).await;
    let client = connect(&engine.addr_string()).await;
    let r = call(&client, "profile_counters", json!({})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    assert!(text(&r).contains("\"triangles\":3400"));
    assert_eq!(
        engine.methods().last().map(String::as_str),
        Some("profile.counters")
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn find_performance_problem_prompt_names_the_target() {
    let engine = MockEngine::start(None, profile_handler(1)).await;
    let client = connect(&engine.addr_string()).await;
    let args = json!({"symptom": "stutters when turning", "target_fps": "120"});
    let prompt = client
        .get_prompt(
            GetPromptRequestParams::new("find-performance-problem")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .unwrap();
    let ContentBlock::Text(t) = &prompt.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(t.text.contains("The symptom: stutters when turning"));
    assert!(t.text.contains("The target: 120 fps"), "{}", t.text);
    assert!(
        t.text.contains("1000 / 120 ms as `budget_ms`"),
        "{}",
        t.text
    );
    let at = |s: &str| t.text.find(s).unwrap_or_else(|| panic!("missing {s}"));
    assert!(at("profile_capture") < at("profile_counters"));

    let bare = client
        .get_prompt(GetPromptRequestParams::new("find-performance-problem"))
        .await
        .unwrap();
    let ContentBlock::Text(b) = &bare.messages[0].content else {
        panic!("text prompt expected");
    };
    assert!(!b.text.contains("The symptom:") && !b.text.contains("The target:"));
    assert!(!b.text.contains("{{") && !b.text.contains("{%"));
    client.cancel().await.unwrap();
}
