//! quat-mcp: a Model Context Protocol server for the quat-3d engine.
//!
//! ```text
//! AI client ──MCP (stdio)──▶ quat-mcp ──JSON-RPC 2.0 (loopback TCP)──▶ engine
//! ```
//!
//! - [`client`]: the engine's line-delimited JSON-RPC client (lazy connect,
//!   `session.hello`, id matching, timeouts, reconnect).
//! - [`server`]: the MCP tools, prompts and resources.
//! - [`params`]: typed tool inputs (their doc comments are the schemas the
//!   model reads).
//! - [`config`]: command line / environment configuration.

pub mod client;
pub mod config;
pub mod params;
pub mod server;

pub use client::{EngineClient, EngineConfig, EngineError};
pub use server::QuatServer;
