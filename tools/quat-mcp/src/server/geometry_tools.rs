//! Runtime geometry tools (ROADMAP R23.2): generator specs (roofs, wall
//! sweeps, extrusions, stones), 2D polygon operations and the dynamic mesh
//! store. Descriptions live in `content/tools/<tool>.md`; the engine methods
//! are documented in docs/remote-control.md ("Runtime geometry").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::to_params;
use crate::params::{
    DynamicMeshIdParams, DynamicMeshSetParams, DynamicMeshUpdateParams, GeometryGenerateParams,
    GeometryPolygonParams,
};

#[tool_router(router = geometry_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/geometry_generate.md")]
    #[tool]
    async fn geometry_generate(
        &self,
        params: Parameters<GeometryGenerateParams>,
    ) -> CallToolResult {
        self.run("geometry.generate", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/geometry_polygon.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn geometry_polygon(&self, params: Parameters<GeometryPolygonParams>) -> CallToolResult {
        self.run("geometry.polygon", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/mesh_dynamic_list.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn mesh_dynamic_list(&self) -> CallToolResult {
        self.run("mesh.dynamic.list", None).await
    }

    #[doc = include_str!("../../content/tools/mesh_dynamic_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn mesh_dynamic_get(&self, params: Parameters<DynamicMeshIdParams>) -> CallToolResult {
        self.run("mesh.dynamic.get", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/mesh_dynamic_set.md")]
    #[tool]
    async fn mesh_dynamic_set(&self, params: Parameters<DynamicMeshSetParams>) -> CallToolResult {
        self.run("mesh.dynamic.set", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/mesh_dynamic_update_vertices.md")]
    #[tool]
    async fn mesh_dynamic_update_vertices(
        &self,
        params: Parameters<DynamicMeshUpdateParams>,
    ) -> CallToolResult {
        self.run("mesh.dynamic.update_vertices", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/mesh_dynamic_remove.md")]
    #[tool(annotations(destructive_hint = true))]
    async fn mesh_dynamic_remove(&self, params: Parameters<DynamicMeshIdParams>) -> CallToolResult {
        self.run("mesh.dynamic.remove", Some(to_params(&params.0)))
            .await
    }
}
