//! Command-line / environment configuration.

use std::path::PathBuf;
use std::time::Duration;

use clap::Parser;

use crate::client::{ENGINE_MAX_MESSAGE_BYTES, EngineConfig, token_file};

/// MCP server for the quat-3d engine. Speaks MCP over stdio; connects to the
/// engine's remote-control endpoint (start the engine with `QUAT_REMOTE`=<port>).
#[derive(Debug, Clone, Parser)]
#[command(name = "quat-mcp", version, about, long_about = None)]
pub struct Cli {
    /// Engine host. The engine listens on loopback only.
    #[arg(long, env = "QUAT_REMOTE_HOST", default_value = "127.0.0.1")]
    pub host: String,

    /// Engine remote-control port (the engine's `QUAT_REMOTE` value).
    #[arg(long, env = "QUAT_REMOTE_PORT", default_value_t = 7878)]
    pub port: u16,

    /// Session token, if the engine was started with `QUAT_REMOTE_TOKEN`.
    /// Without it, the token is read from the engine's token file.
    #[arg(long, env = "QUAT_REMOTE_TOKEN", hide_env_values = true)]
    pub token: Option<String>,

    /// The engine's session token file (default: the per-user file for
    /// `--port`; the engine honours the same `QUAT_REMOTE_TOKEN_FILE`).
    #[arg(long, env = "QUAT_REMOTE_TOKEN_FILE")]
    pub token_file: Option<PathBuf>,

    /// Per-request timeout in seconds (also bounds connecting).
    #[arg(long, env = "QUAT_MCP_TIMEOUT_SECS", default_value_t = 15.0)]
    pub timeout_secs: f64,

    /// Timeout in seconds for slow requests: screenshots, batches, model
    /// spawning, scene load/save/clear and navmesh baking.
    #[arg(long, env = "QUAT_MCP_LONG_TIMEOUT_SECS", default_value_t = 120.0)]
    pub long_timeout_secs: f64,

    /// Largest reply accepted from the engine, in MiB.
    #[arg(long, env = "QUAT_MCP_MAX_REPLY_MB", default_value_t = 64)]
    pub max_reply_mb: usize,
}

impl Cli {
    #[must_use]
    pub fn engine_config(&self) -> EngineConfig {
        let mut c = EngineConfig::new(format!("{}:{}", self.host, self.port));
        c.token = self.token.clone().filter(|t| !t.is_empty());
        c.token_file = self
            .token_file
            .clone()
            .or_else(|| token_file::default_path(self.port));
        c.timeout = secs(self.timeout_secs, 15.0);
        c.max_request_bytes = ENGINE_MAX_MESSAGE_BYTES;
        c.max_reply_bytes = self.max_reply_mb.clamp(1, 4096) * 1024 * 1024;
        c
    }

    #[must_use]
    pub fn long_timeout(&self) -> Duration {
        secs(self.long_timeout_secs, 120.0)
    }
}

fn secs(v: f64, fallback: f64) -> Duration {
    Duration::from_secs_f64(if v.is_finite() && v > 0.0 {
        v
    } else {
        fallback
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults() {
        let cli = Cli::try_parse_from(["quat-mcp"]).unwrap();
        let c = cli.engine_config();
        assert!(c.addr.ends_with(":7878") || std::env::var("QUAT_REMOTE_PORT").is_ok());
        assert_eq!(c.max_request_bytes, 16 * 1024 * 1024);
    }

    #[test]
    fn flags_override() {
        let cli = Cli::try_parse_from([
            "quat-mcp",
            "--host",
            "localhost",
            "--port",
            "9000",
            "--token",
            "",
            "--timeout-secs",
            "2.5",
        ])
        .unwrap();
        let c = cli.engine_config();
        assert_eq!(c.addr, "localhost:9000");
        assert_eq!(c.token, None, "empty token means none");
        assert_eq!(c.timeout, Duration::from_millis(2500));
    }

    #[test]
    fn token_file_defaults_to_the_ports_file_and_can_be_overridden() {
        let cli = Cli::try_parse_from(["quat-mcp", "--port", "9100"]).unwrap();
        assert!(
            cli.engine_config().token_file == token_file::default_path(9100)
                || std::env::var("QUAT_REMOTE_TOKEN_FILE").is_ok()
        );
        let cli = Cli::try_parse_from(["quat-mcp", "--token-file", "/tmp/engine.token"]).unwrap();
        assert_eq!(
            cli.engine_config().token_file,
            Some(PathBuf::from("/tmp/engine.token"))
        );
    }
}
