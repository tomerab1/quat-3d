//! Engine client against the mock engine: hello/auth, id matching, timeouts,
//! reconnects, error mapping and size limits.

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Action, MockEngine};
use quat_mcp::client::code;
use quat_mcp::{EngineClient, EngineConfig, EngineError};
use serde_json::{Value, json};

fn client(addr: &str, token: Option<&str>) -> EngineClient {
    let mut c = EngineConfig::new(addr);
    c.token = token.map(str::to_owned);
    c.timeout = Duration::from_secs(5);
    EngineClient::new(c)
}

fn echo(method: &str, params: &Value) -> Action {
    match method {
        "echo" => Action::Reply(params.clone()),
        "slow_echo" => Action::Delayed(
            Duration::from_millis(params["delay_ms"].as_u64().unwrap_or(0)),
            params.clone(),
        ),
        "hang" => Action::Silent,
        "drop" => Action::Disconnect,
        "big" => Action::Reply(json!(
            "x".repeat(usize::try_from(params["len"].as_u64().unwrap()).unwrap())
        )),
        "fail" => Action::ErrorWithData(
            code::INVALID_PARAMS,
            "point_light.radius: expected a number >= 0".into(),
            json!({"field": "point_light.radius"}),
        ),
        _ => Action::Error(code::METHOD_NOT_FOUND, format!("unknown method '{method}'")),
    }
}

#[tokio::test]
async fn connects_lazily_and_says_hello_first() {
    let engine = MockEngine::start(None, echo).await;
    let c = client(&engine.addr_string(), None);
    assert_eq!(
        engine.connections(),
        0,
        "no connection before the first call"
    );
    assert!(!c.is_connected().await);

    let v = c.call("echo", Some(json!({"a": 1}))).await.unwrap();
    assert_eq!(v, json!({"a": 1}));
    assert_eq!(engine.methods(), ["session.hello", "echo"]);
    assert_eq!(engine.last_params("session.hello"), Some(json!({})));
    let hello = c.hello_info().unwrap();
    assert_eq!(hello.protocol, Some(1));
    assert_eq!(hello.server.as_deref(), Some("quat-3d"));

    // Later calls reuse the connection.
    c.call("echo", None).await.unwrap();
    assert_eq!(engine.connections(), 1);
    assert_eq!(engine.methods().len(), 3);
}

#[tokio::test]
async fn token_is_sent_in_hello() {
    let engine = MockEngine::start(Some("secret"), echo).await;
    let c = client(&engine.addr_string(), Some("secret"));
    assert_eq!(
        c.call("echo", Some(json!({"x": true}))).await.unwrap(),
        json!({"x": true})
    );
    assert_eq!(
        engine.last_params("session.hello"),
        Some(json!({"token": "secret"}))
    );
}

#[tokio::test]
async fn wrong_or_missing_token_is_unauthorized() {
    let engine = MockEngine::start(Some("secret"), echo).await;

    let wrong = client(&engine.addr_string(), Some("nope"));
    let err = wrong.call("echo", None).await.unwrap_err();
    assert!(matches!(err, EngineError::Unauthorized { .. }), "{err:?}");
    assert_eq!(err.rpc_code(), Some(code::UNAUTHORIZED));
    let text = err.to_string();
    assert!(text.contains("invalid token"), "{text}");
    assert!(text.contains("QUAT_REMOTE_TOKEN"), "{text}");
    assert!(!engine.methods().contains(&"echo".to_owned()));

    // No token configured: hello is sent without one and refused.
    let missing = client(&engine.addr_string(), None);
    let err = missing.call("echo", None).await.unwrap_err();
    assert!(matches!(err, EngineError::Unauthorized { .. }), "{err:?}");
}

#[tokio::test]
async fn concurrent_replies_are_matched_by_id() {
    let engine = MockEngine::start(None, echo).await;
    let c = Arc::new(client(&engine.addr_string(), None));
    c.call("echo", None).await.unwrap(); // connect first

    // Replies arrive in the reverse order of the requests.
    let mut tasks = Vec::new();
    for i in 0..8u64 {
        let c = Arc::clone(&c);
        tasks.push(tokio::spawn(async move {
            let params = json!({"i": i, "delay_ms": (8 - i) * 25});
            let v = c.call("slow_echo", Some(params)).await.unwrap();
            assert_eq!(v["i"], i);
        }));
    }
    for t in tasks {
        t.await.unwrap();
    }
    assert_eq!(engine.connections(), 1);
}

#[tokio::test]
async fn timeout_then_late_reply_is_discarded() {
    let engine = MockEngine::start(None, echo).await;
    let c = client(&engine.addr_string(), None);

    let err = c
        .call_with_timeout("hang", None, Duration::from_millis(150))
        .await
        .unwrap_err();
    assert!(matches!(err, EngineError::Timeout { .. }), "{err:?}");
    assert!(
        err.to_string()
            .contains("hang: no reply from the engine within 0.1s")
    );

    // A reply that arrives after its timeout must not be delivered to the next call.
    let err = c
        .call_with_timeout(
            "slow_echo",
            Some(json!({"delay_ms": 300, "tag": "late"})),
            Duration::from_millis(100),
        )
        .await
        .unwrap_err();
    assert!(matches!(err, EngineError::Timeout { .. }));
    tokio::time::sleep(Duration::from_millis(350)).await;
    let v = c.call("echo", Some(json!({"tag": "fresh"}))).await.unwrap();
    assert_eq!(v["tag"], "fresh");
    assert_eq!(
        engine.connections(),
        1,
        "a timeout does not drop the connection"
    );
}

#[tokio::test]
async fn reconnects_after_the_engine_drops_the_connection() {
    let engine = MockEngine::start(None, echo).await;
    let c = client(&engine.addr_string(), None);
    c.call("echo", None).await.unwrap();

    engine.drop_connections();
    tokio::time::sleep(Duration::from_millis(50)).await;
    assert!(!c.is_connected().await);

    let v = c
        .call("echo", Some(json!({"after": "drop"})))
        .await
        .unwrap();
    assert_eq!(v["after"], "drop");
    assert_eq!(engine.connections(), 2);
    // The new connection greeted the engine again.
    assert_eq!(
        engine
            .methods()
            .iter()
            .filter(|m| *m == "session.hello")
            .count(),
        2
    );
}

#[tokio::test]
async fn in_flight_call_fails_when_the_connection_drops() {
    let engine = MockEngine::start(None, echo).await;
    let c = client(&engine.addr_string(), None);
    let err = c.call("drop", None).await.unwrap_err();
    assert!(matches!(err, EngineError::Disconnected { .. }), "{err:?}");
    assert!(err.to_string().contains("next call reconnects"));
    // And the next call works.
    assert_eq!(c.call("echo", Some(json!(1))).await.unwrap(), json!(1));
}

#[tokio::test]
async fn engine_down_then_up() {
    // Reserve a port, then free it so nothing listens there.
    let probe = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = probe.local_addr().unwrap().to_string();
    drop(probe);

    let c = client(&addr, None);
    let err = c.call("echo", None).await.unwrap_err();
    assert!(matches!(err, EngineError::Connect { .. }), "{err:?}");
    let text = err.to_string();
    assert!(
        text.contains("cannot connect to the quat-3d engine"),
        "{text}"
    );
    assert!(text.contains("QUAT_REMOTE="), "{text}");

    let engine = MockEngine::start_on(&addr, None, echo).await;
    assert_eq!(c.call("echo", Some(json!(2))).await.unwrap(), json!(2));

    // The engine quits and comes back on the same port.
    engine.shutdown();
    tokio::time::sleep(Duration::from_millis(50)).await;
    assert!(matches!(
        c.call("echo", None).await.unwrap_err(),
        EngineError::Connect { .. } | EngineError::Disconnected { .. }
    ));
    let _engine = MockEngine::start_on(&addr, None, echo).await;
    assert_eq!(c.call("echo", Some(json!(3))).await.unwrap(), json!(3));
}

#[tokio::test]
async fn engine_errors_keep_code_message_and_data() {
    let engine = MockEngine::start(None, echo).await;
    let c = client(&engine.addr_string(), None);
    match c.call("fail", None).await.unwrap_err() {
        EngineError::Rpc {
            method,
            code,
            message,
            data,
        } => {
            assert_eq!(method, "fail");
            assert_eq!(code, -32602);
            assert_eq!(message, "point_light.radius: expected a number >= 0");
            assert_eq!(data, Some(json!({"field": "point_light.radius"})));
        }
        other => panic!("unexpected {other:?}"),
    }
    let err = c.call("nope.nothing", None).await.unwrap_err();
    assert_eq!(err.rpc_code(), Some(code::METHOD_NOT_FOUND));
    assert!(
        err.to_string()
            .contains("method_not_found (-32601): unknown method 'nope.nothing'")
    );
}

#[tokio::test]
async fn malformed_reply_is_a_protocol_error() {
    // The raw line carries no result/error; give it the id the client will use
    // (1 = hello, 2 = this call).
    let engine = MockEngine::start(None, |m: &str, _: &Value| {
        if m == "garbage" {
            Action::Raw(r#"{"jsonrpc":"2.0","id":2}"#.into())
        } else {
            Action::Reply(json!(null))
        }
    })
    .await;
    let c = client(&engine.addr_string(), None);
    let err = c.call("garbage", None).await.unwrap_err();
    assert!(matches!(err, EngineError::Protocol { .. }), "{err:?}");
}

#[tokio::test]
async fn oversized_request_is_refused_locally() {
    let engine = MockEngine::start(None, echo).await;
    let mut cfg = EngineConfig::new(engine.addr_string());
    cfg.max_request_bytes = 1024;
    let c = EngineClient::new(cfg);
    let err = c
        .call("echo", Some(json!({"blob": "y".repeat(2000)})))
        .await
        .unwrap_err();
    assert!(
        matches!(err, EngineError::RequestTooLarge { .. }),
        "{err:?}"
    );
    assert!(!engine.methods().contains(&"echo".to_owned()));
}

#[tokio::test]
async fn oversized_reply_drops_the_connection_then_recovers() {
    let engine = MockEngine::start(None, echo).await;
    let mut cfg = EngineConfig::new(engine.addr_string());
    cfg.max_reply_bytes = 4096;
    let c = EngineClient::new(cfg);
    let err = c
        .call("big", Some(json!({"len": 10_000})))
        .await
        .unwrap_err();
    assert!(matches!(err, EngineError::Disconnected { .. }), "{err:?}");
    assert!(err.to_string().contains("reply limit"), "{err}");
    let v = c.call("big", Some(json!({"len": 10}))).await.unwrap();
    assert_eq!(v, json!("x".repeat(10)));
    assert_eq!(engine.connections(), 2);
}
