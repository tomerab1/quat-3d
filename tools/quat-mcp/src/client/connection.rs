//! One TCP connection to the engine: a writer, a reader task that routes
//! replies to waiting calls by id, and the bookkeeping for in-flight calls.

use std::collections::HashMap;
use std::sync::{Arc, Mutex as StdMutex};
use std::time::Duration;

use futures::StreamExt;
use serde_json::Value;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::sync::{Mutex, oneshot};
use tokio::task::JoinHandle;
use tokio_util::codec::{FramedRead, LinesCodec, LinesCodecError};

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
pub(super) enum Failure {
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

pub(super) struct Connection {
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
    pub(super) fn spawn(stream: TcpStream, max_reply_bytes: usize) -> Self {
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

    pub(super) fn is_closed(&self) -> bool {
        lock(&self.pending).closed.is_some()
    }

    pub(super) fn close(&self, reason: &str) {
        lock(&self.pending).close(reason);
        self.reader.abort();
    }

    /// Send one request line and wait for its reply.
    pub(super) async fn request(
        &self,
        id: u64,
        line: &str,
        timeout: Duration,
    ) -> Result<Value, Failure> {
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
