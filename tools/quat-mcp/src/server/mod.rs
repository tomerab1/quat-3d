//! The MCP server: one tool per engine method (docs/remote-control.md, v1
//! contract) grouped by area, the workflows as skills and prompts
//! (`crate::skills`), and the scene-summary and scripting resources.
//! Everything the model reads is Markdown under `content/`.

// Tool modules attach `content/tools/*.md` as doc attributes, so clippy's
// rustdoc lints would police that Markdown. It is text for the model, not API
// documentation: `doc_markdown` (backticks around identifiers) is off there.
#[allow(clippy::doc_markdown)]
mod asset_tools;
#[allow(clippy::doc_markdown)]
mod behavior_tools;
mod bt_outline;
#[allow(clippy::doc_markdown)]
mod debug_tools;
mod results;
#[allow(clippy::doc_markdown)]
mod scene_tools;
#[allow(clippy::doc_markdown)]
mod script_tools;
#[allow(clippy::doc_markdown)]
mod shader_tools;
#[allow(clippy::doc_markdown)]
mod sim_tools;
#[allow(clippy::doc_markdown)]
mod tuning_tools;
#[allow(clippy::doc_markdown)]
mod weather_tools;
#[allow(clippy::doc_markdown)]
mod world_tools;

use std::sync::Arc;
use std::time::Duration;

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::model::{
    CallToolResult, GetPromptRequestMethod, GetPromptRequestParams, GetPromptResponse,
    GetSkillRequestMethod, GetSkillRequestParams, GetSkillResult, Implementation,
    ListPromptsRequestMethod, ListPromptsResult, ListResourcesResult, ListSkillsRequestMethod,
    ListSkillsResult, PaginatedRequestParams, PromptsCapability, ReadResourceRequestParams,
    ReadResourceResponse, ReadResourceResult, Resource, ResourceContents, ServerCapabilities,
    ServerConfig,
};
use rmcp::service::RequestContext;
use rmcp::{ErrorData, RoleServer, ServerHandler, tool_handler};
use serde_json::{Value, json};

use crate::client::{EngineClient, EngineError};
use crate::content;
use crate::skills::{self, prompts};

pub use asset_tools::AssetConfig;
pub use results::{error_result, normalize_method};
pub use world_tools::MAX_GROUND_POINTS;

/// URI of the scene-summary resource.
pub const SCENE_SUMMARY_URI: &str = "quat://scene/summary";
/// URI of the Lua scripting reference resource.
pub const SCRIPTING_REFERENCE_URI: &str = "quat://docs/scripting";

/// The quat-mcp server. Cheap to clone; all clones share one engine connection.
#[derive(Clone)]
pub struct QuatServer {
    engine: Arc<EngineClient>,
    long_timeout: Duration,
    assets: Arc<asset_tools::AssetService>,
    workflows: Workflows,
    instructions: String,
    tool_router: ToolRouter<Self>,
}

/// Which surfaces serve the workflows in `content/skills/`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Workflows {
    /// The MCP skills extension: `skills/list`, `skills/get` and the
    /// `skill://` resources.
    pub skills: bool,
    /// One MCP prompt per skill.
    pub prompts: bool,
}

impl Default for Workflows {
    fn default() -> Self {
        Self {
            skills: true,
            prompts: true,
        }
    }
}

impl std::fmt::Debug for QuatServer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("QuatServer")
            .field("engine", &self.engine.config())
            .field("long_timeout", &self.long_timeout)
            .field("workflows", &self.workflows)
            .finish_non_exhaustive()
    }
}

impl QuatServer {
    #[must_use]
    pub fn new(engine: Arc<EngineClient>, long_timeout: Duration) -> Self {
        Self {
            engine,
            long_timeout,
            assets: asset_tools::AssetService::new(AssetConfig::default()),
            workflows: Workflows::default(),
            instructions: content::instructions(skills::catalog().skills(), Workflows::default()),
            tool_router: Self::tools(),
        }
    }

    /// Serve the workflows as skills and/or prompts (default: both). The
    /// Claude Code plugin ships the skills as files and turns both off.
    #[must_use]
    pub fn with_workflows(mut self, workflows: Workflows) -> Self {
        self.workflows = workflows;
        self.instructions = content::instructions(skills::catalog().skills(), workflows);
        self
    }

    /// Where the asset tools download from and to (default: Poly Haven into
    /// the current directory's `assets/`).
    #[must_use]
    pub fn with_assets(mut self, config: AssetConfig) -> Self {
        self.assets = asset_tools::AssetService::new(config);
        self
    }

    /// Every tool this server offers, across all tool groups.
    #[must_use]
    pub fn tools() -> ToolRouter<Self> {
        Self::scene_tools()
            + Self::world_tools()
            + Self::sim_tools()
            + Self::debug_tools()
            + Self::tuning_tools()
            + Self::behavior_tools()
            + Self::weather_tools()
            + Self::script_tools()
            + Self::asset_tools()
            + Self::shader_tools()
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
        let weather = self.engine.call("weather.get", None).await.ok();
        let camera = self.engine.call("camera.get", None).await.ok();
        Ok(json!({
            "engine": info,
            "scene": list,
            "environment": environment,
            "weather": weather,
            "camera": camera,
        }))
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for QuatServer {
    fn get_info(&self) -> ServerConfig {
        let builder = ServerCapabilities::builder()
            .enable_tools()
            .enable_resources();
        let mut capabilities = if self.workflows.skills {
            builder.enable_skills().build()
        } else {
            builder.build()
        };
        if self.workflows.prompts {
            capabilities.prompts = Some(PromptsCapability::default());
        }
        ServerConfig::new(capabilities)
            .with_server_info(Implementation::new("quat-mcp", env!("CARGO_PKG_VERSION")))
            .with_instructions(self.instructions.clone())
    }

    async fn list_skills(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListSkillsResult, ErrorData> {
        if !self.workflows.skills {
            return Err(ErrorData::method_not_found::<ListSkillsRequestMethod>());
        }
        Ok(skills::catalog().list_skills())
    }

    async fn get_skill(
        &self,
        request: GetSkillRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<GetSkillResult, ErrorData> {
        if !self.workflows.skills {
            return Err(ErrorData::method_not_found::<GetSkillRequestMethod>());
        }
        skills::catalog().get_skill(&request.uri)
    }

    async fn list_prompts(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListPromptsResult, ErrorData> {
        if !self.workflows.prompts {
            return Err(ErrorData::method_not_found::<ListPromptsRequestMethod>());
        }
        Ok(ListPromptsResult::with_all_items(prompts::prompts().list()))
    }

    async fn get_prompt(
        &self,
        request: GetPromptRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<GetPromptResponse, ErrorData> {
        if !self.workflows.prompts {
            return Err(ErrorData::method_not_found::<GetPromptRequestMethod>());
        }
        prompts::prompts()
            .get(&request.name, request.arguments.as_ref())
            .map(Into::into)
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, ErrorData> {
        let mut resources = vec![
            Resource::new(SCENE_SUMMARY_URI, "scene_summary")
                .with_title("Current scene summary")
                .with_description(include_str!("../../content/resources/scene_summary.md").trim())
                .with_mime_type("application/json"),
            Resource::new(SCRIPTING_REFERENCE_URI, "scripting_reference")
                .with_title("Lua scripting reference")
                .with_description(
                    include_str!("../../content/resources/scripting_reference.md").trim(),
                )
                .with_mime_type("text/markdown"),
        ];
        if self.workflows.skills {
            resources.extend(skills::catalog().resources());
        }
        Ok(ListResourcesResult::with_all_items(resources))
    }

    async fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<ReadResourceResponse, ErrorData> {
        if self.workflows.skills
            && let Some(file) = skills::catalog().read(&request.uri)
        {
            return Ok(file.into());
        }
        if request.uri == SCRIPTING_REFERENCE_URI {
            return Ok(ReadResourceResult::new(vec![
                ResourceContents::text(content::SCRIPTING_REFERENCE, SCRIPTING_REFERENCE_URI)
                    .with_mime_type("text/markdown"),
            ])
            .into());
        }
        if request.uri != SCENE_SUMMARY_URI {
            return Err(ErrorData::resource_not_found(
                format!(
                    "unknown resource `{}`; available: {SCENE_SUMMARY_URI}, \
                     {SCRIPTING_REFERENCE_URI}{}",
                    request.uri,
                    if self.workflows.skills {
                        format!(", {}", skills::catalog().uris())
                    } else {
                        String::new()
                    }
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
