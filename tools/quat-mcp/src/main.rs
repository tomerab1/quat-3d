use std::sync::Arc;

use anyhow::Context;
use clap::Parser;
use rmcp::ServiceExt;
use tracing_subscriber::EnvFilter;

use quat_mcp::config::Cli;
use quat_mcp::{EngineClient, QuatServer};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // stdout carries the MCP transport: logs go to stderr only.
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .with_env_filter(
            EnvFilter::try_from_env("QUAT_MCP_LOG").unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .init();

    let cli = Cli::parse();
    let config = cli.engine_config();
    tracing::info!(
        "quat-mcp {} serving MCP on stdio; engine at {}{}",
        env!("CARGO_PKG_VERSION"),
        config.addr,
        if config.token.is_some() {
            " (with token)"
        } else {
            ""
        }
    );

    // The engine connection is opened lazily by the first tool call, so the
    // MCP client can start quat-mcp before the engine is running.
    let engine = Arc::new(EngineClient::new(config));
    let service = QuatServer::new(engine, cli.long_timeout())
        .serve(rmcp::transport::stdio())
        .await
        .context("starting the MCP server on stdio")?;
    let reason = service.waiting().await.context("MCP session failed")?;
    tracing::info!("MCP session ended: {reason:?}");
    Ok(())
}
