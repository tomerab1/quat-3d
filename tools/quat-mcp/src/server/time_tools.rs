//! World-clock tools (ROADMAP R22.1): date, time and place of the scene,
//! which drive the sun. Descriptions live in `content/tools/<tool>.md`; the
//! engine methods are documented in docs/remote-control.md ("World clock").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::to_params;
use crate::params::{TimeSetParams, TimeSkipParams};

#[tool_router(router = time_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/time_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn time_get(&self) -> CallToolResult {
        self.run("time.get", None).await
    }

    #[doc = include_str!("../../content/tools/time_set.md")]
    #[tool]
    async fn time_set(&self, params: Parameters<TimeSetParams>) -> CallToolResult {
        self.run("time.set", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/time_skip.md")]
    #[tool]
    async fn time_skip(&self, params: Parameters<TimeSkipParams>) -> CallToolResult {
        self.run("time.skip", Some(to_params(&params.0))).await
    }
}
