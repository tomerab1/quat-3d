//! Game-packaging tools (ROADMAP P4): the open game project and its
//! background build. Descriptions live in `content/tools/<tool>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{ProjectBuildParams, ProjectBuildStatusParams};

/// Most log lines `project_build_status` may ask for.
const MAX_LOG_LINES: u32 = 1000;

#[tool_router(router = project_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/project_info.md")]
    #[tool]
    async fn project_info(&self) -> CallToolResult {
        self.run("project.info", None).await
    }

    // Destructive: a rebuild replaces the package of the same name in `output`.
    #[doc = include_str!("../../content/tools/project_build.md")]
    #[tool]
    async fn project_build(&self, params: Parameters<ProjectBuildParams>) -> CallToolResult {
        self.run("project.build", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/project_build_status.md")]
    #[tool]
    async fn project_build_status(
        &self,
        params: Parameters<ProjectBuildStatusParams>,
    ) -> CallToolResult {
        if params.0.log_lines.is_some_and(|n| n > MAX_LOG_LINES) {
            return invalid_input("project_build_status: log_lines must be at most 1000.");
        }
        self.run("project.build_status", Some(to_params(&params.0)))
            .await
    }
}
