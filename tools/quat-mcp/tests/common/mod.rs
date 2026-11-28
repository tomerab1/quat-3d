//! A mock quat-3d engine: a tokio TCP server speaking the newline-delimited
//! JSON-RPC 2.0 remote-control protocol (docs/remote-control.md), with
//! scriptable per-method behaviour.

#![allow(dead_code)]

use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{mpsc, watch};
use tokio::task::JoinHandle;

/// What the mock does with one request.
pub enum Action {
    /// Reply with this `result`.
    Reply(Value),
    /// Reply with this error code and message.
    Error(i64, String),
    /// Reply with an error that carries `data`.
    ErrorWithData(i64, String, Value),
    /// Reply after a delay (other requests are served meanwhile).
    Delayed(Duration, Value),
    /// Never reply.
    Silent,
    /// Close the connection without replying.
    Disconnect,
    /// Write this raw line instead of a proper reply.
    Raw(String),
}

pub type Handler = dyn Fn(&str, &Value) -> Action + Send + Sync;

pub struct MockEngine {
    pub addr: SocketAddr,
    state: Arc<State>,
    accept: JoinHandle<()>,
}

struct State {
    token: Option<String>,
    handler: Box<Handler>,
    /// Every request received: (method, params).
    requests: Mutex<Vec<(String, Value)>>,
    connections: AtomicUsize,
    /// Bumped to make every open connection close.
    kill: watch::Sender<u64>,
}

impl MockEngine {
    /// Start on an ephemeral port.
    pub async fn start(
        token: Option<&str>,
        handler: impl Fn(&str, &Value) -> Action + Send + Sync + 'static,
    ) -> Self {
        Self::start_on("127.0.0.1:0", token, handler).await
    }

    /// Start on a specific address (e.g. to bring an engine "back up").
    pub async fn start_on(
        addr: &str,
        token: Option<&str>,
        handler: impl Fn(&str, &Value) -> Action + Send + Sync + 'static,
    ) -> Self {
        let listener = TcpListener::bind(addr).await.expect("bind mock engine");
        let addr = listener.local_addr().unwrap();
        let (kill, _) = watch::channel(0);
        let state = Arc::new(State {
            token: token.map(str::to_owned),
            handler: Box::new(handler),
            requests: Mutex::new(Vec::new()),
            connections: AtomicUsize::new(0),
            kill,
        });
        let st = Arc::clone(&state);
        let accept = tokio::spawn(async move {
            while let Ok((sock, _)) = listener.accept().await {
                st.connections.fetch_add(1, Ordering::SeqCst);
                tokio::spawn(serve_connection(sock, Arc::clone(&st)));
            }
        });
        Self {
            addr,
            state,
            accept,
        }
    }

    pub fn addr_string(&self) -> String {
        self.addr.to_string()
    }

    /// Methods received so far, in order.
    pub fn methods(&self) -> Vec<String> {
        self.state
            .requests
            .lock()
            .unwrap()
            .iter()
            .map(|(m, _)| m.clone())
            .collect()
    }

    /// Params of the last request for `method`.
    pub fn last_params(&self, method: &str) -> Option<Value> {
        self.state
            .requests
            .lock()
            .unwrap()
            .iter()
            .rev()
            .find(|(m, _)| m == method)
            .map(|(_, p)| p.clone())
    }

    pub fn connections(&self) -> usize {
        self.state.connections.load(Ordering::SeqCst)
    }

    /// Close every open connection (the listener keeps accepting).
    pub fn drop_connections(&self) {
        self.state.kill.send_modify(|g| *g += 1);
    }

    /// Stop accepting and close every connection ("engine quit").
    pub fn shutdown(self) {
        self.accept.abort();
        self.drop_connections();
    }
}

async fn serve_connection(sock: TcpStream, state: Arc<State>) {
    let (read, mut write) = sock.into_split();
    let mut lines = BufReader::new(read).lines();
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    let writer = tokio::spawn(async move {
        while let Some(line) = rx.recv().await {
            if write.write_all(line.as_bytes()).await.is_err() {
                break;
            }
        }
    });
    let mut kill = state.kill.subscribe();
    let mut authed = state.token.is_none();

    loop {
        let line = tokio::select! {
            l = lines.next_line() => match l {
                Ok(Some(l)) => l,
                _ => break,
            },
            _ = kill.changed() => break,
        };
        let Ok(req) = serde_json::from_str::<Value>(&line) else {
            send(
                &tx,
                &json!({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"invalid JSON"}}),
            );
            continue;
        };
        let id = req["id"].clone();
        let method = req["method"].as_str().unwrap_or_default().to_owned();
        let params = req.get("params").cloned().unwrap_or(Value::Null);
        state
            .requests
            .lock()
            .unwrap()
            .push((method.clone(), params.clone()));

        if method == "session.hello" {
            let given = params.get("token").and_then(Value::as_str);
            if state.token.is_some() && given != state.token.as_deref() {
                send(&tx, &error(&id, -32001, "invalid token"));
                break;
            }
            authed = true;
            send(
                &tx,
                &json!({"jsonrpc":"2.0","id":id,"result":{"protocol":1,"server":"quat-3d","methods":30}}),
            );
            continue;
        }
        if !authed {
            send(
                &tx,
                &error(&id, -32001, "session.hello with a token required first"),
            );
            break;
        }

        match (state.handler)(&method, &params) {
            Action::Reply(result) => send(&tx, &json!({"jsonrpc":"2.0","id":id,"result":result})),
            Action::Error(code, msg) => send(&tx, &error(&id, code, &msg)),
            Action::ErrorWithData(code, msg, data) => send(
                &tx,
                &json!({"jsonrpc":"2.0","id":id,"error":{"code":code,"message":msg,"data":data}}),
            ),
            Action::Delayed(delay, result) => {
                let tx = tx.clone();
                tokio::spawn(async move {
                    tokio::time::sleep(delay).await;
                    send(&tx, &json!({"jsonrpc":"2.0","id":id,"result":result}));
                });
            }
            Action::Silent => {}
            Action::Disconnect => break,
            Action::Raw(raw) => {
                let _ = tx.send(format!("{raw}\n"));
            }
        }
    }
    drop(tx);
    // Let queued replies flush before the socket closes.
    let _ = tokio::time::timeout(Duration::from_millis(200), writer).await;
}

fn error(id: &Value, code: i64, message: &str) -> Value {
    json!({"jsonrpc":"2.0","id":id,"error":{"code":code,"message":message}})
}

fn send(tx: &mpsc::UnboundedSender<String>, msg: &Value) {
    let _ = tx.send(format!("{msg}\n"));
}

/// A handler implementing a tiny in-memory scene, enough for tool tests.
pub fn scene_handler() -> impl Fn(&str, &Value) -> Action + Send + Sync + 'static {
    let next_id = AtomicUsize::new(1);
    move |method, params| match method {
        "engine.info" => Action::Reply(
            json!({"engine":"quat-3d","frame":812,"entities":3,"playing":false,"editor":true}),
        ),
        "scene.create" => {
            let id = next_id.fetch_add(1, Ordering::SeqCst);
            let name = params
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or("Entity");
            Action::Reply(json!({"id": id, "name": name}))
        }
        "scene.update" => {
            if params
                .pointer("/components/point_light/radius")
                .and_then(Value::as_f64)
                < Some(0.0)
            {
                Action::Error(-32602, "point_light.radius: expected a number >= 0".into())
            } else {
                Action::Reply(json!({"id": params["entity"]}))
            }
        }
        "scene.list" => Action::Reply(json!({"total": 1, "entities": [
            {"id": 1, "name": "Sun", "parent": null, "components": ["transform", "dir_light"],
             "world_position": [0, 10, 0]}]})),
        "scene.get" => Action::Error(-32003, format!("entity {} not found", params["entity"])),
        "scene.batch" => {
            let results: Vec<Value> = params["commands"]
                .as_array()
                .map(|cmds| {
                    cmds.iter()
                        .enumerate()
                        .map(|(i, c)| {
                            if c["method"] == "scene.create" {
                                json!({"ok": true, "result": {"id": 100 + i}})
                            } else {
                                json!({"ok": false, "error": {"code": -32601,
                                    "message": format!("unknown method {}", c["method"])}})
                            }
                        })
                        .collect()
                })
                .unwrap_or_default();
            Action::Reply(json!({ "results": results }))
        }
        "render.screenshot" => Action::Reply(json!({
            "mime": "image/png", "width": 2, "height": 1,
            // Not a real PNG; the server passes the base64 through untouched.
            "data": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAEUlEQVR4nGP4z8DwHwyBBAA2+wT8dVn4cAAAAABJRU5ErkJggg=="
        })),
        "environment.get" => Action::Error(-32002, "clouds need the full application".into()),
        "camera.get" => {
            Action::Reply(json!({"position":[0,2,5],"yaw_deg":-90,"pitch_deg":0,"fov_deg":60}))
        }
        _ => Action::Error(-32601, format!("unknown method '{method}'")),
    }
}

// ---- MCP client ----------------------------------------------------------

/// The protocol the end-to-end suite speaks: `QUAT_MCP_TEST_PROTOCOL`, or
/// 2025-11-25 (the `initialize` handshake). CI runs the suite once more with
/// `QUAT_MCP_TEST_PROTOCOL=2026-07-28` (stateless `server/discover`).
pub fn protocol() -> rmcp::model::ProtocolVersion {
    match std::env::var("QUAT_MCP_TEST_PROTOCOL").ok().as_deref() {
        None | Some("" | "2025-11-25") => rmcp::model::ProtocolVersion::V_2025_11_25,
        Some("2026-07-28") => rmcp::model::ProtocolVersion::V_2026_07_28,
        Some(other) => panic!("QUAT_MCP_TEST_PROTOCOL={other}: use 2025-11-25 or 2026-07-28"),
    }
}

/// Connect `handler` as an MCP client over `io`, speaking `version`.
pub async fn client_with<S, T>(
    handler: S,
    io: T,
    version: rmcp::model::ProtocolVersion,
) -> rmcp::service::RunningService<rmcp::RoleClient, S>
where
    S: rmcp::Service<rmcp::RoleClient>,
    T: tokio::io::AsyncRead + tokio::io::AsyncWrite + Send + 'static,
{
    use rmcp::ServiceExt as _;
    use rmcp::service::{ClientLifecycleMode, ClientServiceExt as _};
    if version.has_initialize() {
        handler.serve(io).await.expect("client connects")
    } else {
        handler
            .serve_with_lifecycle(
                io,
                ClientLifecycleMode::Discover {
                    preferred_versions: vec![version],
                },
            )
            .await
            .expect("client connects")
    }
}

/// Connect a plain client (no elicitation) over `io`, speaking [`protocol`].
pub async fn client<T>(io: T) -> rmcp::service::RunningService<rmcp::RoleClient, ()>
where
    T: tokio::io::AsyncRead + tokio::io::AsyncWrite + Send + 'static,
{
    client_with((), io, protocol()).await
}
