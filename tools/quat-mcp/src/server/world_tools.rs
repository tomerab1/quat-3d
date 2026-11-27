//! World tools: sky, terrain heights, camera, screenshots and assets.
//! Descriptions live in `content/tools/<tool>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde::Deserialize;

use super::QuatServer;
use super::results::{error_result, invalid_input, to_params};
use crate::params::{
    CameraFrameParams, CameraSetParams, EnvironmentSetParams, GroundHeightParams, ScreenshotParams,
    SpawnModelParams,
};

/// Most `world_ground_height` points one call may carry (contract limit).
pub const MAX_GROUND_POINTS: usize = 1024;

/// The engine's `render.screenshot` reply.
#[derive(Deserialize)]
struct Screenshot {
    data: String,
    #[serde(default = "default_mime")]
    mime: String,
    #[serde(default)]
    width: u64,
    #[serde(default)]
    height: u64,
}

fn default_mime() -> String {
    "image/png".to_owned()
}

impl QuatServer {
    /// Capture the viewport and return it as MCP image content.
    pub(super) async fn capture_screenshot(&self, params: ScreenshotParams) -> CallToolResult {
        let reply = match self
            .engine
            .call_with_timeout(
                "render.screenshot",
                Some(to_params(&params)),
                self.long_timeout(),
            )
            .await
        {
            Ok(v) => v,
            Err(e) => return error_result(&e),
        };
        let Ok(shot) = serde_json::from_value::<Screenshot>(reply) else {
            return invalid_input(
                "render.screenshot: the engine reply has no `data` field with the image; this is \
                 an engine bug.",
            );
        };
        let caption = format!(
            "Viewport screenshot, {}x{} px ({}).",
            shot.width, shot.height, shot.mime
        );
        CallToolResult::success(vec![
            ContentBlock::image(shot.data, shot.mime),
            ContentBlock::text(caption),
        ])
    }
}

#[tool_router(router = world_tools, vis = "pub(super)")]
impl QuatServer {
    // ---- environment / world ----------------------------------------------

    #[doc = include_str!("../../content/tools/environment_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn environment_get(&self) -> CallToolResult {
        self.run("environment.get", None).await
    }

    #[doc = include_str!("../../content/tools/environment_set.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn environment_set(&self, params: Parameters<EnvironmentSetParams>) -> CallToolResult {
        self.run("environment.set", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/world_ground_height.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn world_ground_height(&self, params: Parameters<GroundHeightParams>) -> CallToolResult {
        let n = params.0.points.len();
        if n == 0 {
            return invalid_input("world_ground_height: `points` is empty; give [[x, z], ...].");
        }
        if n > MAX_GROUND_POINTS {
            return invalid_input(format!(
                "world_ground_height: {n} points given, at most {MAX_GROUND_POINTS} per call; \
                 split the query."
            ));
        }
        self.run("world.ground_height", Some(to_params(&params.0)))
            .await
    }

    // ---- camera / screenshot ----------------------------------------------

    #[doc = include_str!("../../content/tools/camera_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn camera_get(&self) -> CallToolResult {
        self.run("camera.get", None).await
    }

    #[doc = include_str!("../../content/tools/camera_set.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn camera_set(&self, params: Parameters<CameraSetParams>) -> CallToolResult {
        self.run("camera.set", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/camera_frame.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn camera_frame(&self, params: Parameters<CameraFrameParams>) -> CallToolResult {
        self.run("camera.frame", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/screenshot.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn screenshot(&self, params: Parameters<ScreenshotParams>) -> CallToolResult {
        self.capture_screenshot(params.0).await
    }

    // ---- assets -----------------------------------------------------------

    #[doc = include_str!("../../content/tools/asset_list_models.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn asset_list_models(&self) -> CallToolResult {
        self.run("asset.list_models", None).await
    }

    #[doc = include_str!("../../content/tools/asset_spawn_model.md")]
    #[doc = include_str!("../../content/components.md")]
    #[tool]
    async fn asset_spawn_model(&self, params: Parameters<SpawnModelParams>) -> CallToolResult {
        self.run_long("asset.spawn_model", Some(to_params(&params.0)))
            .await
    }
}
