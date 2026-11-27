//! Simulation, scene files and navigation tools.
//! Descriptions live in `content/tools/<tool>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::to_params;
use crate::params::ScenePathParams;

#[tool_router(router = sim_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/sim_play.md")]
    #[tool]
    async fn sim_play(&self) -> CallToolResult {
        self.run("sim.play", None).await
    }

    #[doc = include_str!("../../content/tools/sim_stop.md")]
    #[tool]
    async fn sim_stop(&self) -> CallToolResult {
        self.run("sim.stop", None).await
    }

    #[doc = include_str!("../../content/tools/scene_save.md")]
    #[tool]
    async fn scene_save(&self, params: Parameters<ScenePathParams>) -> CallToolResult {
        self.run_long("scene.save", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/scene_load.md")]
    #[tool(annotations(destructive_hint = true))]
    async fn scene_load(&self, params: Parameters<ScenePathParams>) -> CallToolResult {
        self.run_long("scene.load", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/nav_bake.md")]
    #[tool]
    async fn nav_bake(&self) -> CallToolResult {
        self.run_long("nav.bake", None).await
    }
}
