//! Spline tools (ROADMAP G18). Descriptions live in `content/tools/<tool>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::to_params;
use crate::params::{
    SplineClosestParams, SplineCrossingsParams, SplineEntityParams, SplineFitParams,
    SplineNetworkParams, SplineOffsetParams, SplineSampleParams, SplineSetParams,
};

#[tool_router(router = spline_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/spline_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn spline_get(&self, params: Parameters<SplineEntityParams>) -> CallToolResult {
        self.run("spline.get", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/spline_set.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn spline_set(&self, params: Parameters<SplineSetParams>) -> CallToolResult {
        self.run("spline.set", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/spline_sample.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn spline_sample(&self, params: Parameters<SplineSampleParams>) -> CallToolResult {
        self.run("spline.sample", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/spline_closest.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn spline_closest(&self, params: Parameters<SplineClosestParams>) -> CallToolResult {
        self.run("spline.closest", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/spline_crossings.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn spline_crossings(&self, params: Parameters<SplineCrossingsParams>) -> CallToolResult {
        self.run("spline.crossings", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/spline_offset.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn spline_offset(&self, params: Parameters<SplineOffsetParams>) -> CallToolResult {
        self.run("spline.offset", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/spline_network.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn spline_network(&self, params: Parameters<SplineNetworkParams>) -> CallToolResult {
        self.run("spline.network", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/spline_fit.md")]
    #[tool]
    async fn spline_fit(&self, params: Parameters<SplineFitParams>) -> CallToolResult {
        self.run("spline.fit", Some(to_params(&params.0))).await
    }
}

#[cfg(test)]
mod tests {
    use crate::params::{SplineFitParams, SplinePoint, SplineSetParams, StrokePoint};
    use serde_json::json;

    #[test]
    fn points_accept_both_forms_and_serialize_as_the_engine_reads_them() {
        let p: SplineSetParams = serde_json::from_value(json!({
            "entity": 3, "kind": "polyline",
            "points": [[0, 0, 0], {"position": [1, 0, 0], "width": 2.5, "tags": 4}]
        }))
        .unwrap();
        let pts = p.points.as_ref().unwrap();
        assert!(matches!(pts[0], SplinePoint::Position(_)));
        assert!(matches!(pts[1], SplinePoint::Full(_)));
        let v = serde_json::to_value(&p).unwrap();
        assert_eq!(v["kind"], "polyline");
        assert_eq!(v["points"][1]["width"], 2.5);
        assert!(v["points"][1].get("height").is_none());
        assert!(v.get("closed").is_none());
        assert!(
            serde_json::from_value::<SplineSetParams>(json!({"entity": 1, "kind": "bspline"}))
                .is_err()
        );
    }

    #[test]
    fn stroke_points_carry_the_straight_flag() {
        let p: SplineFitParams = serde_json::from_value(json!({
            "points": [[0, 0, 0], {"position": [4, 0, 0], "straight": true}],
            "output": "cubic_bezier"
        }))
        .unwrap();
        assert!(matches!(p.points[1], StrokePoint::Full(_)));
        let v = serde_json::to_value(&p).unwrap();
        assert_eq!(v["points"][1]["straight"], true);
        assert_eq!(v["output"], "cubic_bezier");
        assert!(v.get("entity").is_none());
    }
}
