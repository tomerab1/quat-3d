//! Terrain edit tools (ROADMAP G19.1): ordered stamps that flatten, raise,
//! lower, smooth or paint the generated terrain. Descriptions live in
//! `content/tools/<tool>.md`; the engine methods are documented in
//! docs/remote-control.md ("Terrain edits").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{
    MAX_STAMP_POINTS, TerrainEditAddParams, TerrainEditRemoveParams, TerrainEditUpdateParams,
};

/// The precise error for a stamp with too many points, if it has.
fn too_many_points(tool: &str, points: Option<&Vec<[f64; 2]>>) -> Option<CallToolResult> {
    let n = points.map_or(0, Vec::len);
    (n > MAX_STAMP_POINTS).then(|| {
        invalid_input(format!(
            "{tool}: {n} points given, at most {MAX_STAMP_POINTS} per stamp; simplify the \
             outline or split it into several stamps."
        ))
    })
}

#[tool_router(router = terrain_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/terrain_edit_add.md")]
    #[tool(annotations(destructive_hint = false, idempotent_hint = false))]
    async fn terrain_edit_add(&self, params: Parameters<TerrainEditAddParams>) -> CallToolResult {
        if let Some(err) = too_many_points("terrain_edit_add", Some(&params.0.points)) {
            return err;
        }
        self.run("terrain.edit.add", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/terrain_edit_update.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn terrain_edit_update(
        &self,
        params: Parameters<TerrainEditUpdateParams>,
    ) -> CallToolResult {
        if params.0.is_empty() {
            return invalid_input(format!(
                "terrain_edit_update: nothing to change for stamp {}; give at least one stamp \
                 field besides id, e.g. {{\"id\": {}, \"height\": 12.5}}.",
                params.0.id, params.0.id
            ));
        }
        if let Some(err) = too_many_points("terrain_edit_update", params.0.points.as_ref()) {
            return err;
        }
        self.run("terrain.edit.update", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/terrain_edit_remove.md")]
    #[tool(annotations(destructive_hint = true, idempotent_hint = true))]
    async fn terrain_edit_remove(
        &self,
        params: Parameters<TerrainEditRemoveParams>,
    ) -> CallToolResult {
        match (&params.0.id, &params.0.ids) {
            (None, None) => {
                return invalid_input(
                    "terrain_edit_remove: give the stamp to remove as `id` (e.g. {\"id\": 4}) \
                     or several as `ids` (e.g. {\"ids\": [4, 7]}); `terrain_edit_list` shows them.",
                );
            }
            (Some(_), Some(_)) => {
                return invalid_input(
                    "terrain_edit_remove: give either `id` or `ids`, not both; put every id \
                     in `ids`.",
                );
            }
            (None, Some(ids)) if ids.is_empty() => {
                return invalid_input(
                    "terrain_edit_remove: `ids` is empty; list at least one stamp id.",
                );
            }
            _ => {}
        }
        self.run("terrain.edit.remove", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/terrain_edit_list.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn terrain_edit_list(&self) -> CallToolResult {
        self.run("terrain.edit.list", None).await
    }

    #[doc = include_str!("../../content/tools/terrain_edit_clear.md")]
    #[tool(annotations(destructive_hint = true, idempotent_hint = true))]
    async fn terrain_edit_clear(&self) -> CallToolResult {
        self.run("terrain.edit.clear", None).await
    }
}
