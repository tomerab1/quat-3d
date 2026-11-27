//! Asset tools: search and download CC0 models from Poly Haven into the
//! project (ROADMAP A9, issue #42). They run here in quat-mcp, not in the
//! engine; `asset_spawn_model` then loads what they fetched.
//! Descriptions live in `content/tools/<tool>.md`.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::invalid_input;
use crate::assets::{
    AssetError, FetchOptions, HttpError, POLYHAVEN_API, PolyHaven, ReqwestHttp, fetch,
};
use crate::params::{AssetFetchParams, AssetSearchParams};

/// Where the asset tools download from and to.
#[derive(Debug, Clone)]
pub struct AssetConfig {
    /// The quat-3d checkout (holds `assets/`).
    pub project_root: PathBuf,
    /// Poly Haven API base URL.
    pub api: String,
    /// Largest download per model, in bytes.
    pub max_bytes: u64,
    /// Per-request HTTP timeout.
    pub timeout: Duration,
}

impl Default for AssetConfig {
    fn default() -> Self {
        Self {
            project_root: std::env::current_dir().unwrap_or_default(),
            api: POLYHAVEN_API.to_owned(),
            max_bytes: 512 * 1024 * 1024,
            timeout: Duration::from_secs(300),
        }
    }
}

/// The configured client (or why there is none).
#[derive(Debug)]
pub(super) struct AssetService {
    config: AssetConfig,
    client: Result<PolyHaven<ReqwestHttp>, String>,
}

impl AssetService {
    pub(super) fn new(config: AssetConfig) -> Arc<Self> {
        let client = ReqwestHttp::new(config.timeout)
            .map(|http| PolyHaven::new(http, config.api.clone()))
            .map_err(|e| format!("the HTTP client could not start: {e}"));
        Arc::new(Self { config, client })
    }
}

/// An asset error as a tool error, with what to do about it.
fn asset_error(tool: &str, e: &AssetError) -> CallToolResult {
    let hint = match e {
        AssetError::Http(HttpError::Unreachable { .. }) => {
            "\nHint: Poly Haven cannot be reached from this machine (offline, or blocked by a \
             proxy or firewall). asset_list_models lists the models already in the project; \
             primitives and instanced meshes need no downloads."
        }
        AssetError::Http(HttpError::Status { status, .. }) if *status >= 500 => {
            "\nHint: Poly Haven is having trouble; try again later."
        }
        _ => "",
    };
    invalid_input(format!("{tool} failed: {e}{hint}"))
}

impl QuatServer {
    fn polyhaven(&self) -> Result<&PolyHaven<ReqwestHttp>, CallToolResult> {
        self.assets
            .client
            .as_ref()
            .map_err(|e| invalid_input(format!("asset tools are unavailable: {e}")))
    }
}

#[tool_router(router = asset_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/asset_search.md")]
    #[tool(annotations(read_only_hint = true, open_world_hint = true))]
    async fn asset_search(&self, params: Parameters<AssetSearchParams>) -> CallToolResult {
        let ph = match self.polyhaven() {
            Ok(ph) => ph,
            Err(e) => return e,
        };
        let p = params.0;
        let limit = p.limit.unwrap_or(10).clamp(1, 25) as usize;
        match ph.search(&p.query, p.category.as_deref(), limit).await {
            Ok(result) => CallToolResult::success(vec![ContentBlock::text(
                serde_json::to_string(&result).unwrap_or_default(),
            )]),
            Err(e) => asset_error("asset_search", &e),
        }
    }

    #[doc = include_str!("../../content/tools/asset_fetch.md")]
    #[tool(annotations(idempotent_hint = true, open_world_hint = true))]
    async fn asset_fetch(&self, params: Parameters<AssetFetchParams>) -> CallToolResult {
        let ph = match self.polyhaven() {
            Ok(ph) => ph,
            Err(e) => return e,
        };
        let p = params.0;
        let opts = FetchOptions {
            resolution: p.resolution.unwrap_or_default().as_str().to_owned(),
            max_bytes: self.assets.config.max_bytes,
            overwrite: p.overwrite.unwrap_or(false),
            cook: p.cook.unwrap_or(true),
        };
        match fetch(ph, &self.assets.config.project_root, &p.id, &opts).await {
            Ok(report) => CallToolResult::success(vec![ContentBlock::text(
                serde_json::to_string(&report).unwrap_or_default(),
            )]),
            Err(e) => asset_error("asset_fetch", &e),
        }
    }
}
