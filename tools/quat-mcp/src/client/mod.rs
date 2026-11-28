//! Client for the engine's remote-control endpoint (`docs/remote-control.md`).
//!
//! Wire format: newline-delimited JSON-RPC 2.0 over a loopback TCP socket.
//! One [`EngineClient`] multiplexes any number of concurrent calls over one
//! connection, matching replies to requests by id.
//!
//! Connection lifecycle:
//! - The connection is opened lazily by the first call, and `session.hello`
//!   carrying the session token is sent before anything else. The token is
//!   the configured one, else the one the engine publishes in its token file
//!   ([`token_file`]), read again for every connection.
//! - When the engine goes away (closed, restarted, crashed) every in-flight
//!   call fails with [`EngineError::Disconnected`] and the *next* call opens a
//!   fresh connection. A request that provably never reached the engine (the
//!   connection was already dead when it was written) is retried once on the
//!   fresh connection; one that may have been applied is never retried.
//! - A call that gets no reply within its timeout fails with
//!   [`EngineError::Timeout`]; a late reply is discarded by id.

mod connection;
mod error;
pub mod token_file;

use std::fmt;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;

use serde_json::{Map, Value, json};
use tokio::net::TcpStream;
use tokio::sync::Mutex;

use connection::{Connection, Failure};
pub use error::{EngineError, code, code_name};

/// Remote-control protocol version this client was written against.
pub const PROTOCOL_VERSION: u64 = 1;

/// The engine refuses request lines larger than this (16 MiB).
pub const ENGINE_MAX_MESSAGE_BYTES: usize = 16 * 1024 * 1024;

/// Default limit for a single reply line. Screenshots are the largest replies
/// (a base64 PNG); 64 MiB leaves ample headroom.
pub const DEFAULT_MAX_REPLY_BYTES: usize = 64 * 1024 * 1024;

/// Settings for [`EngineClient`].
#[derive(Clone)]
pub struct EngineConfig {
    /// `host:port` of the engine endpoint.
    pub addr: String,
    /// Session token (`QUAT_REMOTE_TOKEN`); sent with `session.hello`.
    pub token: Option<String>,
    /// Where to read the session token when `token` is `None`: the engine's
    /// token file ([`token_file::default_path`] or `QUAT_REMOTE_TOKEN_FILE`).
    pub token_file: Option<PathBuf>,
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
            .field("token_file", &self.token_file)
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
            token_file: None,
            timeout: Duration::from_secs(15),
            max_request_bytes: ENGINE_MAX_MESSAGE_BYTES,
            max_reply_bytes: DEFAULT_MAX_REPLY_BYTES,
        }
    }
}

/// Where the token sent in `session.hello` came from; it decides the hint of
/// an [`EngineError::Unauthorized`].
enum TokenSource {
    Configured,
    File(PathBuf),
    Missing(Option<PathBuf>),
}

impl TokenSource {
    fn unauthorized_hint(&self, port: &str) -> String {
        match self {
            TokenSource::Configured => "quat-mcp sent the token from QUAT_REMOTE_TOKEN (--token); \
                it must match the engine's QUAT_REMOTE_TOKEN. Unset it to use the token the \
                engine publishes in its token file."
                .to_owned(),
            TokenSource::File(path) => format!(
                "quat-mcp sent the token from {}, which belongs to another engine run. Restart \
                 the engine and check that QUAT_REMOTE_PORT ({port}) matches its QUAT_REMOTE port.",
                path.display()
            ),
            TokenSource::Missing(Some(path)) => format!(
                "quat-mcp found no session token: {} does not exist. The engine writes it when \
                 started with QUAT_REMOTE={port}; otherwise set QUAT_REMOTE_TOKEN (or \
                 QUAT_REMOTE_TOKEN_FILE) for quat-mcp to match the engine.",
                path.display()
            ),
            TokenSource::Missing(None) => "quat-mcp found no session token (there is no \
                per-user directory for the token file). Set QUAT_REMOTE_TOKEN or \
                QUAT_REMOTE_TOKEN_FILE for quat-mcp to match the engine."
                .to_owned(),
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

/// JSON-RPC client for one engine endpoint. Cheap to share behind an `Arc`;
/// all methods take `&self` and may be called concurrently.
pub struct EngineClient {
    config: EngineConfig,
    conn: Mutex<Option<Arc<Connection>>>,
    next_id: AtomicU64,
    hello: StdMutex<Option<HelloInfo>>,
}

impl EngineClient {
    #[must_use]
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
    ///
    /// # Errors
    /// See [`EngineClient::call_with_timeout`].
    pub async fn call(&self, method: &str, params: Option<Value>) -> Result<Value, EngineError> {
        self.call_with_timeout(method, params, self.config.timeout)
            .await
    }

    /// Call `method`; `params` must be a JSON object (or `None`).
    ///
    /// # Errors
    /// Connecting or the session handshake fails, the request is too large,
    /// the engine does not reply within `timeout`, the connection drops, or
    /// the engine answers with a JSON-RPC error ([`EngineError::Rpc`]).
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
                hint: "The next call opens a new session.".to_owned(),
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

    /// The token for a new session and where it came from.
    async fn session_token(&self) -> Result<(Option<String>, TokenSource), EngineError> {
        if let Some(token) = &self.config.token {
            return Ok((Some(token.clone()), TokenSource::Configured));
        }
        let Some(path) = &self.config.token_file else {
            return Ok((None, TokenSource::Missing(None)));
        };
        Ok(match token_file::read(path).await? {
            Some(token) => (Some(token), TokenSource::File(path.clone())),
            None => (None, TokenSource::Missing(Some(path.clone()))),
        })
    }

    async fn connect(&self) -> Result<Connection, EngineError> {
        let addr = &self.config.addr;
        let port = addr.rsplit(':').next().unwrap_or("7878");
        let connect_err = |reason: String| EngineError::Connect {
            addr: addr.clone(),
            port: port.to_owned(),
            reason,
        };
        let stream = match tokio::time::timeout(self.config.timeout, TcpStream::connect(addr)).await
        {
            Ok(Ok(s)) => s,
            Ok(Err(e)) => return Err(connect_err(e.to_string())),
            Err(_) => return Err(connect_err("timed out".into())),
        };
        // Read after connecting: a running engine has written its token file.
        let (token, source) = self.session_token().await?;
        let conn = Connection::spawn(stream, self.config.max_reply_bytes);

        // Greet first: the engine requires the session token before anything
        // else, and the reply confirms we are talking to a quat-3d engine with
        // a protocol we understand.
        let mut params = Map::new();
        if let Some(token) = token {
            params.insert("token".into(), Value::String(token));
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
                    hint: source.unauthorized_hint(port),
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
            Err(Failure::NotSent(reason) | Failure::Lost(reason)) => {
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
