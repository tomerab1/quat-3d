//! CC0 model search and download from Poly Haven (ROADMAP A9, issue #42).
//! Runs in quat-mcp, not the engine: the engine only ever loads files that
//! are already in the project.
//!
//! - [`http`]: the download trait and its reqwest client.
//! - [`polyhaven`]: API types and search.
//! - [`fetch`]: confined, size-capped, verified downloads with provenance.
//! - [`paths`]: id and path validation.

pub mod fetch;
pub mod http;
pub mod paths;
pub mod polyhaven;

#[cfg(test)]
mod tests;

pub use fetch::{FetchOptions, FetchReport, fetch};
pub use http::{Http, HttpError, ReqwestHttp};
pub use polyhaven::{AssetError, PolyHaven, SearchResult};

/// The public API.
pub const POLYHAVEN_API: &str = "https://api.polyhaven.com";
