//! Look-tuning tools (ROADMAP A8): the renderer's settings (exposure, bloom,
//! anti-aliasing, shadows, sky light, clouds) and per-entity material
//! factors. Descriptions live in `content/tools/<tool>.md`; the engine methods
//! are documented in docs/remote-control.md ("Look tuning").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{
    MaterialGetParams, MaterialSetParams, RenderSettingsSetParams, SettingsGroupParams,
};

#[tool_router(router = tuning_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/render_settings_get.md")]
    #[tool]
    async fn render_settings_get(&self, params: Parameters<SettingsGroupParams>) -> CallToolResult {
        self.run("render.settings.get", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/render_settings_set.md")]
    #[tool]
    async fn render_settings_set(
        &self,
        params: Parameters<RenderSettingsSetParams>,
    ) -> CallToolResult {
        if params.0.is_empty() {
            return invalid_input(
                "render_settings_set: nothing to change; give at least one group, e.g. \
                 {\"exposure\": {\"compensation_ev\": 0.5}}.",
            );
        }
        self.run("render.settings.set", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/render_settings_reset.md")]
    #[tool]
    async fn render_settings_reset(
        &self,
        params: Parameters<SettingsGroupParams>,
    ) -> CallToolResult {
        self.run("render.settings.reset", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/render_settings_schema.md")]
    #[tool]
    async fn render_settings_schema(&self) -> CallToolResult {
        self.run("render.settings.schema", None).await
    }

    #[doc = include_str!("../../content/tools/material_get.md")]
    #[tool]
    async fn material_get(&self, params: Parameters<MaterialGetParams>) -> CallToolResult {
        self.run("material.get", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/material_set.md")]
    #[tool]
    async fn material_set(&self, params: Parameters<MaterialSetParams>) -> CallToolResult {
        let Some(engine_params) = params.0.to_engine() else {
            return invalid_input(
                "material_set: nothing to change; give `material` values and/or `revert` field \
                 names.",
            );
        };
        self.run("material.set", Some(engine_params)).await
    }
}
