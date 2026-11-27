//! Engine error codes and [`EngineError`], whose `Display` text is what the
//! language model reads.

use std::time::Duration;

use serde_json::Value;

use super::token_file::TokenFileError;

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
#[must_use]
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

    #[error("the engine at {addr} rejected the session ({message}). {hint}")]
    Unauthorized {
        addr: String,
        message: String,
        hint: String,
    },

    #[error(
        "cannot use the engine's session token file: {0}. Restart the engine to rewrite it, or \
         set QUAT_REMOTE_TOKEN (or QUAT_REMOTE_TOKEN_FILE) for quat-mcp."
    )]
    TokenFile(#[from] TokenFileError),

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
    #[must_use]
    pub fn rpc_code(&self) -> Option<i64> {
        match self {
            EngineError::Rpc { code, .. } => Some(*code),
            EngineError::Unauthorized { .. } => Some(code::UNAUTHORIZED),
            _ => None,
        }
    }
}
