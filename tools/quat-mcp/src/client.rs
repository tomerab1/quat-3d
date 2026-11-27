//! Client for the engine's remote-control endpoint (`docs/remote-control.md`).
//!
//! Wire format: newline-delimited JSON-RPC 2.0 over a loopback TCP socket.
//! One [`EngineClient`] multiplexes any number of concurrent calls over one
//! connection, matching replies to requests by id.
//!
//! Connection lifecycle:
//! - The connection is opened lazily by the first call, and `session.hello`
//!   (carrying the token when one is configured) is sent before anything else.
//! - When the engine goes away (closed, restarted, crashed) every in-flight
//!   call fails with [`EngineError::Disconnected`] and the *next* call opens a
//!   fresh connection. A request that provably never reached the engine (the
//!   connection was already dead when it was written) is retried once on the
//!   fresh connection; one that may have been applied is never retried.
//! - A call that gets no reply within its timeout fails with
//!   [`EngineError::Timeout`]; a late reply is discarded by id.

use std::collections::HashMap;
use std::fmt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;

use futures::StreamExt;
use serde_json::{Map, Value, json};
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::sync::{Mutex, oneshot};
use tokio::task::JoinHandle;
use tokio_util::codec::{FramedRead, LinesCodec, LinesCodecError};

/// Remote-control protocol version this client was written against.
pub const PROTOCOL_VERSION: u64 = 1;

/// The engine refuses request lines larger than this (16 MiB).
pub const ENGINE_MAX_MESSAGE_BYTES: usize = 16 * 1024 * 1024;

/// Default limit for a single reply line. Screenshots are the largest replies
/// (a base64 PNG); 64 MiB leaves ample headroom.
pub const DEFAULT_MAX_REPLY_BYTES: usize = 64 * 1024 * 1024;

/// JSON-RPC error codes used by the engine (`docs/remote-control.md`, Errors).
pub mod code {
    pub const PARSE_ERROR: i64 = -32700;
    pub const INVALID_REQUEST: i64 = -32600;
    pub const METHOD_NOT_FOUND: i64 = -32601;
    pub const INVALID_PARAMS: i64 = -32602;
    pub const INTERNAL_ERROR: i64 = -32603;
    pub const UNAUTHORIZED: i64 = -32001;
    pub const UNAVAILABLE: i64 = -32002;
    pub const NOT_FOUND: i64 = -32003;
    pub const OPERATION_FAILED: i64 = -32004;
}

/// Symbolic name of an engine error code.
pub fn code_name(c: i64) -> &'static str {
    match c {
        code::PARSE_ERROR => "parse_error",
        code::INVALID_REQUEST => "invalid_request",
        code::METHOD_NOT_FOUND => "method_not_found",
        code::INVALID_PARAMS => "invalid_params",
        code::INTERNAL_ERROR => "internal_error",
        code::UNAUTHORIZED => "unauthorized",
        code::UNAVAILABLE => "unavailable",
        code::NOT_FOUND => "not_found",
        code::OPERATION_FAILED => "operation_failed",
        _ => "error",
    }
}

/// Settings for [`EngineClient`].
#[derive(Clone)]
pub struct EngineConfig {
    /// `host:port` of the engine endpoint.
    pub addr: String,
    /// Session token (`QUAT_REMOTE_TOKEN`); sent with `session.hello`.
    pub token: Option<String>,
    /// Default per-request timeout (also bounds connecting + hello).
    pub timeout: Duration,
    /// Largest request line sent; larger requests fail locally.
    pub max_request_bytes: usize,
    /// Largest reply line accepted; a larger one drops the connection.
    pub max_reply_bytes: usize,
}

impl fmt::Debug for EngineConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("EngineConfig")
            .field("addr", &self.addr)
            .field("token", &self.token.as_ref().map(|_| "<redacted>"))
            .field("timeout", &self.timeout)
            .field("max_request_bytes", &self.max_request_bytes)
            .field("max_reply_bytes", &self.max_reply_bytes)
            .finish()
    }
}

impl EngineConfig {
    pub fn new(addr: impl Into<String>) -> Self {
        Self {
            addr: addr.into(),
            token: None,
            timeout: Duration::from_secs(15),
            max_request_bytes: ENGINE_MAX_MESSAGE_BYTES,
            max_reply_bytes: DEFAULT_MAX_REPLY_BYTES,
        }
    }
}

/// Everything that can go wrong talking to the engine. The `Display` text is
/// what the language model reads, so each message says what happened and
/// what to do about it; engine messages are passed through verbatim.
#[derive(Debug, thiserror::Error)]
pub enum EngineError {
    #[error(
        "cannot connect to the quat-3d engine at {addr}: {reason}. Start the engine with the \
         remote-control endpoint enabled (e.g. `QUAT_REMOTE={port} ./build/release/game`) and \
         make sure quat-mcp points at the same port (QUAT_REMOTE_PORT)."
    )]
    Connect {
        addr: String,
        port: String,
        reason: String,
    },

    #[error(
        "the engine at {addr} rejected the session ({message}). Set QUAT_REMOTE_TOKEN for \
         quat-mcp to the same value the engine was started with."
    )]
    Unauthorized { addr: String, message: String },

    #[error(
        "{method}: no reply from the engine within {:.1}s. The engine may be busy (loading, \
         baking) or paused in a debugger; the request may still be applied later. Check the \
         state (e.g. scene_list) before retrying.",
        timeout.as_secs_f64()
    )]
    Timeout { method: String, timeout: Duration },

    #[error(
        "{method}: the connection to the engine was lost before it replied ({reason}); the \
         request may or may not have been applied. The next call reconnects automatically."
    )]
    Disconnected { method: String, reason: String },

    #[error(
        "{method}: the request is {size} bytes, above the {limit}-byte message limit. Split it \
         into smaller requests (e.g. several scene_batch calls)."
    )]
    RequestTooLarge {
        method: String,
        size: usize,
        limit: usize,
    },

    #[error("{method} failed — engine error {} ({code}): {message}", code_name(*code))]
    Rpc {
        method: String,
        code: i64,
        message: String,
        data: Option<Value>,
    },

    #[error("{method}: malformed reply from the engine: {detail}")]
    Protocol { method: String, detail: String },
}

impl EngineError {
    /// The engine's JSON-RPC error code, for engine-reported errors.
    pub fn rpc_code(&self) -> Option<i64> {
        match self {
            EngineError::Rpc { code, .. } => Some(*code),
            EngineError::Unauthorized { .. } => Some(code::UNAUTHORIZED),
            _ => None,
        }
    }
}

/// What `session.hello` reported.
#[derive(Debug, Clone, PartialEq)]
pub struct HelloInfo {
    pub protocol: Option<u64>,
    pub server: Option<String>,
    pub methods: Option<u64>,
}

/// One outcome delivered to a waiting call by the reader task.
enum Reply {
    Result(Value),
    Error {
        code: i64,
        message: String,
        data: Option<Value>,
    },
    Malformed(String),
    Lost(String),
}

#[derive(Default)]
struct Pending {
    waiters: HashMap<u64, oneshot::Sender<Reply>>,
    /// Set once the connection is unusable; the reason is reported to callers.
    closed: Option<String>,
}

impl Pending {
    fn close(&mut self, reason: &str) {
        if self.closed.is_none() {
            self.closed = Some(reason.to_owned());
        }
        for (_, waiter) in self.waiters.drain() {
            let _ = waiter.send(Reply::Lost(reason.to_owned()));
        }
    }
}

/// Why a request on one connection failed (mapped to [`EngineError`] by the client).
enum Failure {
    /// The request never reached the engine; safe to retry on a new connection.
    NotSent(String),
    Lost(String),
    Timeout,
    Rpc {
        code: i64,
        message: String,
        data: Option<Value>,
    },
    Malformed(String),
}

struct Connection {
    writer: Mutex<OwnedWriteHalf>,
    pending: Arc<StdMutex<Pending>>,
    reader: JoinHandle<()>,
}

impl Drop for Connection {
    fn drop(&mut self) {
        self.reader.abort();
    }
}

fn lock(pending: &StdMutex<Pending>) -> std::sync::MutexGuard<'_, Pending> {
    // A poisoned lock only means another thread panicked mid-update; the map
    // itself is still consistent enough to drain.
    pending
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

impl Connection {
    fn spawn(stream: TcpStream, max_reply_bytes: usize) -> Self {
        let _ = stream.set_nodelay(true);
        let (read, write) = stream.into_split();
        let pending = Arc::new(StdMutex::new(Pending::default()));
        let reader = tokio::spawn(read_loop(read, Arc::clone(&pending), max_reply_bytes));
        Self {
            writer: Mutex::new(write),
            pending,
            reader,
        }
    }

    fn is_closed(&self) -> bool {
        lock(&self.pending).closed.is_some()
    }

    fn close(&self, reason: &str) {
        lock(&self.pending).close(reason);
        self.reader.abort();
    }

    /// Send one request line and wait for its reply.
    async fn request(&self, id: u64, line: &str, timeout: Duration) -> Result<Value, Failure> {
        let (tx, rx) = oneshot::channel();
        {
            let mut pending = lock(&self.pending);
            if let Some(reason) = &pending.closed {
                return Err(Failure::NotSent(reason.clone()));
            }
            pending.waiters.insert(id, tx);
        }

        let deadline = tokio::time::Instant::now() + timeout;
        let write = async {
            let mut writer = self.writer.lock().await;
            writer.write_all(line.as_bytes()).await?;
            writer.flush().await
        };
        match tokio::time::timeout_at(deadline, write).await {
            Ok(Ok(())) => {}
            Ok(Err(e)) => {
                // The final newline was not accepted, so the engine never saw
                // a complete request. The connection is unusable either way.
                lock(&self.pending).waiters.remove(&id);
                let reason = format!("write failed: {e}");
                self.close(&reason);
                return Err(Failure::NotSent(reason));
            }
            Err(_) => {
                // A partially written line would corrupt the framing of the
                // next request: drop the connection.
                lock(&self.pending).waiters.remove(&id);
                self.close("timed out while sending a request");
                return Err(Failure::Timeout);
            }
        }

        match tokio::time::timeout_at(deadline, rx).await {
            Ok(Ok(Reply::Result(v))) => Ok(v),
            Ok(Ok(Reply::Error {
                code,
                message,
                data,
            })) => Err(Failure::Rpc {
                code,
                message,
                data,
            }),
            Ok(Ok(Reply::Malformed(detail))) => Err(Failure::Malformed(detail)),
            Ok(Ok(Reply::Lost(reason))) => Err(Failure::Lost(reason)),
            Ok(Err(_)) => Err(Failure::Lost("connection closed".into())),
            Err(_) => {
                lock(&self.pending).waiters.remove(&id);
                Err(Failure::Timeout)
            }
        }
    }
}

async fn read_loop(read: OwnedReadHalf, pending: Arc<StdMutex<Pending>>, max_reply_bytes: usize) {
    let mut lines = FramedRead::new(read, LinesCodec::new_with_max_length(max_reply_bytes));
    let reason = loop {
        match lines.next().await {
            Some(Ok(line)) => dispatch(&line, &pending),
            Some(Err(LinesCodecError::MaxLineLengthExceeded)) => {
                break format!(
                    "the engine sent a message larger than the {} MiB reply limit (raise \
                     QUAT_MCP_MAX_REPLY_MB, or request a smaller screenshot)",
                    max_reply_bytes / (1024 * 1024)
                );
            }
            Some(Err(LinesCodecError::Io(e))) => break format!("read error: {e}"),
            None => break "the engine closed the connection".to_owned(),
        }
    };
    tracing::info!("engine connection closed: {reason}");
    lock(&pending).close(&reason);
}

fn dispatch(line: &str, pending: &StdMutex<Pending>) {
    let line = line.trim();
    if line.is_empty() {
        return;
    }
    let msg: Value = match serde_json::from_str(line) {
        Ok(v) => v,
        Err(e) => {
            tracing::warn!("ignoring unparsable line from the engine: {e}");
            return;
        }
    };
    let Some(obj) = msg.as_object() else {
        tracing::warn!("ignoring non-object message from the engine");
        return;
    };
    let id = match obj.get("id") {
        Some(Value::Number(n)) => n.as_u64(),
        Some(Value::String(s)) => s.parse().ok(),
        _ => None,
    };
    let Some(id) = id else {
        // id null: the engine could not attribute the error to a request.
        if let Some(err) = obj.get("error") {
            tracing::warn!("engine error without a request id: {err}");
        } else if obj.contains_key("method") {
            tracing::debug!("ignoring engine notification: {line}");
        }
        return;
    };
    let Some(waiter) = lock(pending).waiters.remove(&id) else {
        tracing::debug!("discarding reply to request {id} (timed out or unknown)");
        return;
    };
    let reply = if let Some(err) = obj.get("error") {
        let code = err.get("code").and_then(Value::as_i64);
        let message = err.get("message").and_then(Value::as_str);
        match (code, message) {
            (Some(code), Some(message)) => Reply::Error {
                code,
                message: message.to_owned(),
                data: err.get("data").filter(|d| !d.is_null()).cloned(),
            },
            _ => Reply::Malformed(format!("error object without code/message: {err}")),
        }
    } else if let Some(result) = obj.get("result") {
        Reply::Result(result.clone())
    } else {
        Reply::Malformed("reply has neither `result` nor `error`".into())
    };
    let _ = waiter.send(reply);
}

/// JSON-RPC client for one engine endpoint. Cheap to share behind an `Arc`;
/// all methods take `&self` and may be called concurrently.
pub struct EngineClient {
    config: EngineConfig,
    conn: Mutex<Option<Arc<Connection>>>,
    next_id: AtomicU64,
    hello: StdMutex<Option<HelloInfo>>,
}

impl EngineClient {
    pub fn new(config: EngineConfig) -> Self {
        Self {
            config,
            conn: Mutex::new(None),
            next_id: AtomicU64::new(1),
            hello: StdMutex::new(None),
        }
    }

    pub fn config(&self) -> &EngineConfig {
        &self.config
    }

    /// What the engine reported in `session.hello` on the current/last connection.
    pub fn hello_info(&self) -> Option<HelloInfo> {
        self.hello
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }

    /// Whether a live connection is currently open.
    pub async fn is_connected(&self) -> bool {
        self.conn
            .lock()
            .await
            .as_ref()
            .is_some_and(|c| !c.is_closed())
    }

    /// Call `method` with the default timeout.
    pub async fn call(&self, method: &str, params: Option<Value>) -> Result<Value, EngineError> {
        self.call_with_timeout(method, params, self.config.timeout)
            .await
    }

    /// Call `method`; `params` must be a JSON object (or `None`).
    pub async fn call_with_timeout(
        &self,
        method: &str,
        params: Option<Value>,
        timeout: Duration,
    ) -> Result<Value, EngineError> {
        // At most one retry, and only for a request that never left this process.
        let mut attempt = 0;
        loop {
            attempt += 1;
            let conn = self.connection().await?;
            let id = self.next_id.fetch_add(1, Ordering::Relaxed);
            let line = encode_request(id, method, params.as_ref());
            if line.len() > self.config.max_request_bytes {
                return Err(EngineError::RequestTooLarge {
                    method: method.to_owned(),
                    size: line.len(),
                    limit: self.config.max_request_bytes,
                });
            }
            tracing::debug!(id, method, "-> engine");
            match conn.request(id, &line, timeout).await {
                Ok(v) => return Ok(v),
                Err(Failure::NotSent(reason)) if attempt == 1 => {
                    tracing::info!("{method}: connection was dead ({reason}); reconnecting");
                    continue;
                }
                Err(f) => return Err(self.map_failure(method, timeout, f)),
            }
        }
    }

    fn map_failure(&self, method: &str, timeout: Duration, failure: Failure) -> EngineError {
        let method = method.to_owned();
        match failure {
            Failure::NotSent(reason) | Failure::Lost(reason) => {
                EngineError::Disconnected { method, reason }
            }
            Failure::Timeout => EngineError::Timeout { method, timeout },
            Failure::Rpc {
                code: code::UNAUTHORIZED,
                message,
                ..
            } => EngineError::Unauthorized {
                addr: self.config.addr.clone(),
                message,
            },
            Failure::Rpc {
                code,
                message,
                data,
            } => EngineError::Rpc {
                method,
                code,
                message,
                data,
            },
            Failure::Malformed(detail) => EngineError::Protocol { method, detail },
        }
    }

    /// The live connection, opening (and greeting) a new one when needed.
    async fn connection(&self) -> Result<Arc<Connection>, EngineError> {
        let mut slot = self.conn.lock().await;
        if let Some(conn) = slot.as_ref() {
            if !conn.is_closed() {
                return Ok(Arc::clone(conn));
            }
            tracing::info!(
                "engine connection was lost; reconnecting to {}",
                self.config.addr
            );
        }
        *slot = None;
        let conn = Arc::new(self.connect().await?);
        *slot = Some(Arc::clone(&conn));
        Ok(conn)
    }

    async fn connect(&self) -> Result<Connection, EngineError> {
        let addr = &self.config.addr;
        let connect_err = |reason: String| EngineError::Connect {
            addr: addr.clone(),
            port: addr.rsplit(':').next().unwrap_or("7878").to_owned(),
            reason,
        };
        let stream = match tokio::time::timeout(self.config.timeout, TcpStream::connect(addr)).await
        {
            Ok(Ok(s)) => s,
            Ok(Err(e)) => return Err(connect_err(e.to_string())),
            Err(_) => return Err(connect_err("timed out".into())),
        };
        let conn = Connection::spawn(stream, self.config.max_reply_bytes);

        // Greet first: required when the engine has a token, and it confirms
        // we are talking to a quat-3d engine with a protocol we understand.
        let mut params = Map::new();
        if let Some(token) = &self.config.token {
            params.insert("token".into(), Value::String(token.clone()));
        }
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let line = encode_request(id, "session.hello", Some(&Value::Object(params)));
        let hello = match conn.request(id, &line, self.config.timeout).await {
            Ok(v) => v,
            Err(Failure::Rpc {
                code: code::UNAUTHORIZED,
                message,
                ..
            }) => {
                conn.close("unauthorized");
                return Err(EngineError::Unauthorized {
                    addr: addr.clone(),
                    message,
                });
            }
            Err(Failure::Timeout) => {
                conn.close("hello timed out");
                return Err(connect_err(
                    "connected, but the engine did not answer session.hello (is something else \
                     listening on this port?)"
                        .into(),
                ));
            }
            Err(Failure::NotSent(reason)) | Err(Failure::Lost(reason)) => {
                return Err(connect_err(format!(
                    "connection dropped during session.hello: {reason}"
                )));
            }
            Err(Failure::Rpc { code, message, .. }) => {
                conn.close("hello failed");
                return Err(connect_err(format!(
                    "session.hello failed with engine error {} ({code}): {message}",
                    code_name(code)
                )));
            }
            Err(Failure::Malformed(detail)) => {
                conn.close("hello malformed");
                return Err(connect_err(format!(
                    "unexpected session.hello reply: {detail}"
                )));
            }
        };

        let info = HelloInfo {
            protocol: hello.get("protocol").and_then(Value::as_u64),
            server: hello
                .get("server")
                .and_then(Value::as_str)
                .map(str::to_owned),
            methods: hello.get("methods").and_then(Value::as_u64),
        };
        if info.protocol.is_some_and(|p| p != PROTOCOL_VERSION) {
            tracing::warn!(
                "engine speaks remote-control protocol {:?}; quat-mcp was written for {}",
                info.protocol,
                PROTOCOL_VERSION
            );
        }
        tracing::info!(
            "connected to {} at {addr} (protocol {:?}, {:?} methods)",
            info.server.as_deref().unwrap_or("engine"),
            info.protocol,
            info.methods
        );
        *self
            .hello
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(info);
        Ok(conn)
    }
}

fn encode_request(id: u64, method: &str, params: Option<&Value>) -> String {
    let mut req = json!({"jsonrpc": "2.0", "id": id, "method": method});
    if let Some(p) = params {
        req["params"] = p.clone();
    }
    let mut line = req.to_string();
    line.push('\n');
    line
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_encoding_is_one_compact_line() {
        let line = encode_request(7, "scene.get", Some(&json!({"entity": "a\nb"})));
        assert!(line.ends_with('\n'));
        assert_eq!(
            line.matches('\n').count(),
            1,
            "newlines inside strings are escaped"
        );
        let v: Value = serde_json::from_str(line.trim_end()).unwrap();
        assert_eq!(v["jsonrpc"], "2.0");
        assert_eq!(v["id"], 7);
        assert_eq!(v["method"], "scene.get");
        assert_eq!(v["params"]["entity"], "a\nb");
        let bare = encode_request(1, "engine.info", None);
        assert!(!bare.contains("params"));
    }

    #[test]
    fn rpc_error_display_keeps_engine_message() {
        let e = EngineError::Rpc {
            method: "scene.create".into(),
            code: code::INVALID_PARAMS,
            message: "point_light.radius: expected a number >= 0".into(),
            data: None,
        };
        let text = e.to_string();
        assert!(text.contains("invalid_params (-32602)"));
        assert!(text.ends_with("point_light.radius: expected a number >= 0"));
    }

    #[test]
    fn config_debug_redacts_token() {
        let mut c = EngineConfig::new("127.0.0.1:1");
        c.token = Some("hunter2".into());
        assert!(!format!("{c:?}").contains("hunter2"));
    }
}
