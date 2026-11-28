//! Input-action tools (ROADMAP G5) and camera rig tools (ROADMAP G15.1).
//! Descriptions live in `content/tools/<tool>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};
use serde_json::Value;

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{
    CameraRigFrameParams, CameraRigGetParams, CameraRigSetParams, InputActionsParams,
    InputBindParams, InputContextParams, InputInjectParams,
};

/// `camera.rig.set` takes the `camera_rig` fields at the top level: the
/// typed ones plus `more`, flattened. A field given both ways is a mistake.
fn rig_set_params(p: &CameraRigSetParams) -> Result<Value, String> {
    let mut out = to_params(p);
    let Some(obj) = out.as_object_mut() else {
        return Ok(out);
    };
    let more = obj.remove("more");
    if let Some(Value::Object(extra)) = more {
        for (k, v) in extra {
            if obj.contains_key(&k) {
                return Err(format!(
                    "`{k}` is given twice (as a parameter and in `more`)"
                ));
            }
            obj.insert(k, v);
        }
    }
    Ok(out)
}

#[tool_router(router = input_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/input_actions.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn input_actions(&self, params: Parameters<InputActionsParams>) -> CallToolResult {
        self.run("input.actions", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/input_inject.md")]
    #[tool]
    async fn input_inject(&self, params: Parameters<InputInjectParams>) -> CallToolResult {
        self.run("input.inject", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/input_bind.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn input_bind(&self, params: Parameters<InputBindParams>) -> CallToolResult {
        self.run("input.bind", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/input_context.md")]
    #[tool]
    async fn input_context(&self, params: Parameters<InputContextParams>) -> CallToolResult {
        self.run("input.context", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/camera_rig_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn camera_rig_get(&self, params: Parameters<CameraRigGetParams>) -> CallToolResult {
        self.run("camera.rig.get", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/camera_rig_set.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn camera_rig_set(&self, params: Parameters<CameraRigSetParams>) -> CallToolResult {
        match rig_set_params(&params.0) {
            Ok(p) => self.run("camera.rig.set", Some(p)).await,
            Err(e) => invalid_input(&e),
        }
    }

    #[doc = include_str!("../../content/tools/camera_rig_frame.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn camera_rig_frame(&self, params: Parameters<CameraRigFrameParams>) -> CallToolResult {
        self.run("camera.rig.frame", Some(to_params(&params.0)))
            .await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn rig_set_flattens_more_and_rejects_duplicates() {
        let p: CameraRigSetParams = serde_json::from_value(json!({
            "yaw_deg": 45, "immediate": true, "more": {"yaw_step_deg": 90, "bounds": {"shape": "rect"}}
        }))
        .unwrap();
        let v = rig_set_params(&p).unwrap();
        assert_eq!(v["yaw_deg"], 45.0);
        assert_eq!(v["yaw_step_deg"], 90);
        assert_eq!(v["bounds"]["shape"], "rect");
        assert!(v.get("more").is_none());

        let dup: CameraRigSetParams =
            serde_json::from_value(json!({"yaw_deg": 45, "more": {"yaw_deg": 10}})).unwrap();
        assert!(rig_set_params(&dup).unwrap_err().contains("yaw_deg"));
    }
}
