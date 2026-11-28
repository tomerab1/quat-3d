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
mod confirm;
#[allow(clippy::doc_markdown)]
mod debug_tools;
mod icons;
#[allow(clippy::doc_markdown)]
mod input_tools;
pub mod outputs;
#[allow(clippy::doc_markdown)]
mod physics;
pub mod policy;
#[allow(clippy::doc_markdown)]
mod profile_tools;
#[allow(clippy::doc_markdown)]
mod project_tools;
mod resources;
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
mod time_tools;
#[allow(clippy::doc_markdown)]
mod tuning_tools;
#[allow(clippy::doc_markdown)]
mod weather_tools;
#[allow(clippy::doc_markdown)]
mod world_tools;

use std::sync::Arc;
use std::time::Duration;

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::tool::ToolCallContext;
use rmcp::model::{
    CallToolRequestParams, CallToolResponse, CallToolResult, CompleteRequestParams, CompleteResult,
    ContentBlock, GetPromptRequestMethod, GetPromptRequestParams, GetPromptResponse,
    GetSkillRequestMethod, GetSkillRequestParams, GetSkillResult, Implementation,
    ListPromptsRequestMethod, ListPromptsResult, ListResourceTemplatesResult, ListResourcesResult,
    ListSkillsRequestMethod, ListSkillsResult, PaginatedRequestParams, PromptsCapability,
    ReadResourceRequestParams, ReadResourceResponse, ServerCapabilities, ServerConfig,
};
use rmcp::service::RequestContext;
use rmcp::{ErrorData, RoleServer, ServerHandler, tool_handler};
use serde_json::{Value, json};

use crate::client::{EngineClient, EngineError};
use crate::content;
use crate::skills::{self, prompts};

pub use asset_tools::AssetConfig;
pub use resources::{ENTITY_TEMPLATE, SCRIPT_TEMPLATE, SHADER_TEMPLATE};
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
    confirmations: Arc<confirm::Confirmations>,
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
            confirmations: Arc::default(),
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

    /// Every tool this server offers, across all tool groups, with the
    /// per-tool policy (annotations, icons, output schemas) applied.
    #[must_use]
    pub fn tools() -> ToolRouter<Self> {
        let mut router = Self::tool_groups();
        policy::apply(&mut router);
        router
    }

    fn tool_groups() -> ToolRouter<Self> {
        Self::scene_tools()
            + Self::world_tools()
            + Self::sim_tools()
            + Self::debug_tools()
            + Self::profile_tools()
            + Self::tuning_tools()
            + Self::behavior_tools()
            + Self::physics_tools()
            + Self::weather_tools()
            + Self::script_tools()
            + Self::asset_tools()
            + Self::shader_tools()
            + Self::project_tools()
            + Self::time_tools()
            + Self::input_tools()
    }

    /// Run a tool call through the router, asking for confirmation first
    /// when its policy says so, then add structured content and resource
    /// links to the result.
    async fn dispatch(
        &self,
        request: CallToolRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, ErrorData> {
        let name = request.name.to_string();
        let policy = policy::get(&name);
        let request = match policy.filter(|p| p.confirm) {
            Some(p) => match self.confirmations.gate(p, request, &context).await {
                confirm::Gate::Proceed(request) => request,
                confirm::Gate::Respond(response) => return Ok(response),
            },
            None => request,
        };
        let response = self
            .tool_router
            .call(ToolCallContext::new(self, request, context))
            .await?;
        Ok(match response {
            CallToolResponse::Complete(result) => finish(&name, result).into(),
            other => other,
        })
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

/// Add `structuredContent` (tools with an output schema) and resource links
/// to a successful result whose text is the engine's JSON reply.
fn finish(name: &str, mut result: CallToolResult) -> CallToolResult {
    if result.is_error == Some(true) {
        return result;
    }
    let reply = result
        .content
        .first()
        .and_then(|c| c.as_text())
        .and_then(|t| serde_json::from_str::<Value>(&t.text).ok());
    if outputs::has_schema(name) {
        let checked = reply
            .as_ref()
            .ok_or_else(|| "the reply is not JSON".to_owned())
            .and_then(|r| outputs::reply_matches(name, r).map(|()| r));
        match checked {
            Ok(reply) => result.structured_content = Some(reply.clone()),
            Err(e) => {
                return CallToolResult::error(vec![ContentBlock::text(format!(
                    "{name}: the engine's reply does not match this tool's output schema ({e}); \
                     the engine and quat-mcp versions may differ. Use engine_call for the raw reply."
                ))]);
            }
        }
    }
    if let Some(reply) = &reply {
        result.content.extend(resources::links(name, reply));
    }
    result
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for QuatServer {
    fn get_info(&self) -> ServerConfig {
        let builder = ServerCapabilities::builder()
            .enable_tools()
            .enable_resources()
            .enable_completions();
        let mut capabilities = if self.workflows.skills {
            builder.enable_skills().build()
        } else {
            builder.build()
        };
        if self.workflows.prompts {
            capabilities.prompts = Some(PromptsCapability::default());
        }
        ServerConfig::new(capabilities)
            .with_server_info(
                Implementation::new("quat-mcp", env!("CARGO_PKG_VERSION"))
                    .with_title("quat-3d")
                    .with_icons(icons::icons(icons::Group::Server).to_vec()),
            )
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

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, ErrorData> {
        self.dispatch(request, context).await
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, ErrorData> {
        let mut resources = resources::fixed();
        if self.workflows.skills {
            resources.extend(skills::catalog().resources());
        }
        Ok(ListResourcesResult::with_all_items(resources))
    }

    async fn list_resource_templates(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourceTemplatesResult, ErrorData> {
        Ok(ListResourceTemplatesResult::with_all_items(
            resources::templates(),
        ))
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
        // SEP-2640: a skill file the server does not serve is invalid params,
        // as for `skills/get`, not resource-not-found.
        if request.uri.starts_with(skills::SCHEME) {
            return Err(ErrorData::invalid_params(
                if self.workflows.skills {
                    format!(
                        "unknown skill file `{}`; available: {}",
                        request.uri,
                        skills::catalog().uris()
                    )
                } else {
                    format!(
                        "unknown skill file `{}`: this server does not serve skills \
                         (started with --skills=false)",
                        request.uri
                    )
                },
                None,
            ));
        }
        self.read_uri(&request.uri).await.map(Into::into)
    }

    async fn complete(
        &self,
        request: CompleteRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<CompleteResult, ErrorData> {
        self.complete_argument(&request).await
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
