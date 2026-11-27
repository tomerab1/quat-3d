//! The MCP server: one tool per engine method (docs/remote-control.md, v1
//! contract) grouped by area, workflow prompts, and a scene-summary resource.
//! Everything the model reads is Markdown under `content/`.

mod debug_tools;
mod prompts;
mod results;
mod scene_tools;
mod sim_tools;
mod world_tools;

use std::sync::Arc;
use std::time::Duration;

use rmcp::handler::server::router::prompt::PromptRouter;
use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::model::{
    CallToolResult, Implementation, ListResourcesResult, PaginatedRequestParams,
    ReadResourceRequestParams, ReadResourceResponse, ReadResourceResult, Resource,
    ResourceContents, ServerCapabilities, ServerConfig,
};
use rmcp::service::RequestContext;
use rmcp::{ErrorData, RoleServer, ServerHandler, prompt_handler, tool_handler};
use serde_json::{Value, json};

use crate::client::{EngineClient, EngineError};
use crate::content;

pub use prompts::{BuildWorldArgs, DebugSessionArgs, InspectSceneArgs};
pub use results::{error_result, normalize_method};
pub use world_tools::MAX_GROUND_POINTS;

/// URI of the scene-summary resource.
pub const SCENE_SUMMARY_URI: &str = "quat://scene/summary";

/// The quat-mcp server. Cheap to clone; all clones share one engine connection.
#[derive(Clone)]
pub struct QuatServer {
    engine: Arc<EngineClient>,
    long_timeout: Duration,
    tool_router: ToolRouter<Self>,
    prompt_router: PromptRouter<Self>,
}

impl std::fmt::Debug for QuatServer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("QuatServer")
            .field("engine", &self.engine.config())
            .field("long_timeout", &self.long_timeout)
            .finish_non_exhaustive()
    }
}

impl QuatServer {
    #[must_use]
    pub fn new(engine: Arc<EngineClient>, long_timeout: Duration) -> Self {
        Self {
            engine,
            long_timeout,
            tool_router: Self::tools(),
            prompt_router: Self::prompt_router(),
        }
    }

    /// Every tool this server offers, across all tool groups.
    #[must_use]
    pub fn tools() -> ToolRouter<Self> {
        Self::scene_tools() + Self::world_tools() + Self::sim_tools() + Self::debug_tools()
    }

    /// Timeout for slow engine work (loading, baking, screenshots).
    fn long_timeout(&self) -> Duration {
        self.long_timeout.max(self.engine.config().timeout)
    }

    async fn run(&self, method: &str, params: Option<Value>) -> CallToolResult {
        match self.engine.call(method, params).await {
            Ok(v) => results::json_result(&v),
            Err(e) => error_result(&e),
        }
    }

    async fn run_long(&self, method: &str, params: Option<Value>) -> CallToolResult {
        match self
            .engine
            .call_with_timeout(method, params, self.long_timeout())
            .await
        {
            Ok(v) => results::json_result(&v),
            Err(e) => error_result(&e),
        }
    }

    async fn scene_summary(&self) -> Result<Value, EngineError> {
        let info = self.engine.call("engine.info", None).await?;
        let list = self
            .engine
            .call("scene.list", Some(json!({"limit": 500})))
            .await?;
        // The sky may be unavailable (headless engine); the summary still works.
        let environment = self.engine.call("environment.get", None).await.ok();
        let camera = self.engine.call("camera.get", None).await.ok();
        Ok(json!({
            "engine": info,
            "scene": list,
            "environment": environment,
            "camera": camera,
        }))
    }
}

#[tool_handler(router = self.tool_router)]
#[prompt_handler(router = self.prompt_router)]
impl ServerHandler for QuatServer {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(
            ServerCapabilities::builder()
                .enable_tools()
                .enable_prompts()
                .enable_resources()
                .build(),
        )
        .with_server_info(Implementation::new("quat-mcp", env!("CARGO_PKG_VERSION")))
        .with_instructions(content::INSTRUCTIONS)
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, ErrorData> {
        Ok(ListResourcesResult::with_all_items(vec![
            Resource::new(SCENE_SUMMARY_URI, "scene_summary")
                .with_title("Current scene summary")
                .with_description(include_str!("../../content/resources/scene_summary.md").trim())
                .with_mime_type("application/json"),
        ]))
    }

    async fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<ReadResourceResponse, ErrorData> {
        if request.uri != SCENE_SUMMARY_URI {
            return Err(ErrorData::resource_not_found(
                format!(
                    "unknown resource `{}`; available: {SCENE_SUMMARY_URI}",
                    request.uri
                ),
                None,
            ));
        }
        let summary = self
            .scene_summary()
            .await
            .map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        let text = serde_json::to_string_pretty(&summary).unwrap_or_else(|_| summary.to_string());
        Ok(ReadResourceResult::new(vec![
            ResourceContents::text(text, SCENE_SUMMARY_URI).with_mime_type("application/json"),
        ])
        .into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_tool_has_an_object_schema_and_description() {
        let tools = QuatServer::tools().list_all();
        assert!(tools.len() >= 27, "{} tools", tools.len());
        for t in &tools {
            assert!(
                t.description.as_deref().is_some_and(|d| d.len() > 30),
                "{} lacks a description",
                t.name
            );
            assert_eq!(
                t.input_schema.get("type"),
                Some(&Value::String("object".into())),
                "{}",
                t.name
            );
        }
    }

    #[test]
    fn every_tool_has_exactly_one_content_file() {
        let names: std::collections::BTreeSet<String> = QuatServer::tools()
            .list_all()
            .into_iter()
            .map(|t| t.name.to_string())
            .collect();
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/content/tools");
        let files: std::collections::BTreeSet<String> = std::fs::read_dir(dir)
            .expect("content/tools exists")
            .filter_map(|e| {
                let path = e.ok()?.path();
                (path.extension()? == "md")
                    .then(|| path.file_stem()?.to_str().map(str::to_owned))?
            })
            .collect();
        assert_eq!(names, files, "tools vs content/tools/*.md");
    }

    #[test]
    fn component_tools_carry_the_component_reference() {
        for t in QuatServer::tools().list_all() {
            let takes_components = matches!(
                t.name.as_ref(),
                "scene_create" | "scene_update" | "scene_batch" | "asset_spawn_model"
            );
            let has_reference = t
                .description
                .as_deref()
                .is_some_and(|d| d.contains("## Components"));
            assert_eq!(takes_components, has_reference, "{}", t.name);
        }
    }
}
