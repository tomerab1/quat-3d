//! Engine and scene tools: status, entity queries and edits, batches.
//! Descriptions live in `content/tools/<tool>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde_json::Value;

use super::QuatServer;
use super::results::{batch_summary, error_result, invalid_input, normalize_method, to_params};
use crate::params::{
    EngineCallParams, EntityParams, RemoveComponentsParams, SceneBatchParams, SceneClearParams,
    SceneCreateParams, SceneListParams, SceneUpdateParams, SetParentParams,
};

#[tool_router(router = scene_tools, vis = "pub(super)")]
impl QuatServer {
    // ---- engine -----------------------------------------------------------

    #[doc = include_str!("../../content/tools/engine_info.md")]
    #[tool]
    async fn engine_info(&self) -> CallToolResult {
        self.run("engine.info", None).await
    }

    #[doc = include_str!("../../content/tools/engine_methods.md")]
    #[tool]
    async fn engine_methods(&self) -> CallToolResult {
        self.run("rpc.methods", None).await
    }

    #[doc = include_str!("../../content/tools/engine_call.md")]
    #[tool]
    async fn engine_call(&self, params: Parameters<EngineCallParams>) -> CallToolResult {
        let p = params.0;
        let method = normalize_method(&p.method);
        if method == "render.screenshot" {
            let screenshot = p
                .params
                .and_then(|m| serde_json::from_value(Value::Object(m)).ok())
                .unwrap_or_default();
            return self.capture_screenshot(screenshot).await;
        }
        self.run_long(&method, p.params.map(Value::Object)).await
    }

    // ---- scene: read ------------------------------------------------------

    #[doc = include_str!("../../content/tools/scene_components.md")]
    #[tool]
    async fn scene_components(&self) -> CallToolResult {
        self.run("scene.components", None).await
    }

    #[doc = include_str!("../../content/tools/scene_list.md")]
    #[tool]
    async fn scene_list(&self, params: Parameters<SceneListParams>) -> CallToolResult {
        self.run("scene.list", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/scene_get.md")]
    #[tool]
    async fn scene_get(&self, params: Parameters<EntityParams>) -> CallToolResult {
        self.run("scene.get", Some(to_params(&params.0))).await
    }

    // ---- scene: write -----------------------------------------------------

    #[doc = include_str!("../../content/tools/scene_create.md")]
    #[doc = include_str!("../../content/components.md")]
    #[tool]
    async fn scene_create(&self, params: Parameters<SceneCreateParams>) -> CallToolResult {
        self.run("scene.create", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/scene_update.md")]
    #[doc = include_str!("../../content/components.md")]
    #[tool]
    async fn scene_update(&self, params: Parameters<SceneUpdateParams>) -> CallToolResult {
        self.run("scene.update", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/scene_remove_components.md")]
    #[tool]
    async fn scene_remove_components(
        &self,
        params: Parameters<RemoveComponentsParams>,
    ) -> CallToolResult {
        self.run("scene.remove_components", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/scene_destroy.md")]
    #[tool]
    async fn scene_destroy(&self, params: Parameters<EntityParams>) -> CallToolResult {
        self.run("scene.destroy", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/scene_set_parent.md")]
    #[tool]
    async fn scene_set_parent(&self, params: Parameters<SetParentParams>) -> CallToolResult {
        self.run("scene.set_parent", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/scene_clear.md")]
    #[tool]
    async fn scene_clear(&self, params: Parameters<SceneClearParams>) -> CallToolResult {
        self.run_long("scene.clear", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/scene_batch.md")]
    #[doc = include_str!("../../content/components.md")]
    #[tool]
    async fn scene_batch(&self, params: Parameters<SceneBatchParams>) -> CallToolResult {
        let mut p = params.0;
        if p.commands.is_empty() {
            return invalid_input("scene_batch: `commands` is empty; give at least one command.");
        }
        for c in &mut p.commands {
            c.method = normalize_method(&c.method);
        }
        let commands = p.commands.len();
        match self
            .engine
            .call_with_timeout("scene.batch", Some(to_params(&p)), self.long_timeout())
            .await
        {
            Ok(v) => {
                let summary = batch_summary(commands, &v).map(ContentBlock::text);
                let content = summary
                    .into_iter()
                    .chain(std::iter::once(ContentBlock::text(v.to_string())))
                    .collect();
                CallToolResult::success(content)
            }
            Err(e) => error_result(&e),
        }
    }
}
