//! quat-mcp: a Model Context Protocol server for the quat-3d engine.
//!
//! ```text
//! AI client ──MCP (stdio)──▶ quat-mcp ──JSON-RPC 2.0 (loopback TCP)──▶ engine
//! ```
//!
//! - [`assets`]: CC0 model search and download from Poly Haven.
//! - [`client`]: the engine's line-delimited JSON-RPC client (lazy connect,
//!   `session.hello`, id matching, timeouts, reconnect).
//! - [`server`]: the MCP tools, prompts, skills and resources.
//! - [`skills`]: the workflow Agent Skills in `content/skills/`, served over
//!   the MCP skills extension and as prompts.
//! - [`content`]: the Markdown the model reads (instructions, tool
//!   descriptions, prompt templates), embedded from `content/`.
//! - [`params`]: typed tool inputs (their doc comments are the schemas the
//!   model reads).
//! - [`config`]: command line / environment configuration.

pub mod assets;
pub mod client;
pub mod config;
pub mod content;
pub mod params;
pub mod server;
pub mod skills;

pub use client::{EngineClient, EngineConfig, EngineError};
pub use server::QuatServer;
