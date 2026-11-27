//! Behaviour-tree authoring tools (ROADMAP A7): build, inspect and edit an
//! entity's behaviour tree — the graph the editor's Behaviour panel shows —
//! and read what it did on its last tick. Descriptions live in
//! `content/tools/<tool>.md`; the engine methods are documented in
//! docs/remote-control.md ("Behaviour-tree authoring").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::{tool, tool_router};
use serde_json::Value;

use super::QuatServer;
use super::bt_outline::{format_status, format_tree_view, labels};
use super::results::{error_result, invalid_input, json_result, to_params};
use crate::params::{
    BtAddNodeParams, BtMoveNodeParams, BtRemoveNodeParams, BtSetParams, BtSetParamsParams,
    BtStatusReply, BtTreeView, EntityParams,
};

impl QuatServer {
    /// Calls a method that answers with a tree view and renders it as an
    /// outline (an unexpected shape, from a newer engine, passes through).
    async fn tree_call(&self, method: &str, params: Value) -> CallToolResult {
        match self.engine.call(method, Some(params)).await {
            Ok(v) => match serde_json::from_value::<BtTreeView>(v.clone()) {
                Ok(view) => {
                    CallToolResult::success(vec![ContentBlock::text(format_tree_view(&view))])
                }
                Err(_) => json_result(&v),
            },
            Err(e) => error_result(&e),
        }
    }
}

#[tool_router(router = behavior_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/bt_leaves.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn bt_leaves(&self) -> CallToolResult {
        self.run("bt.leaves", None).await
    }

    #[doc = include_str!("../../content/tools/bt_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn bt_get(&self, params: Parameters<EntityParams>) -> CallToolResult {
        self.tree_call("bt.get", to_params(&params.0)).await
    }

    #[doc = include_str!("../../content/tools/bt_set.md")]
    #[tool(annotations(destructive_hint = true))]
    async fn bt_set(&self, params: Parameters<BtSetParams>) -> CallToolResult {
        self.tree_call("bt.set", to_params(&params.0)).await
    }

    #[doc = include_str!("../../content/tools/bt_add_node.md")]
    #[tool]
    async fn bt_add_node(&self, params: Parameters<BtAddNodeParams>) -> CallToolResult {
        self.tree_call("bt.add_node", to_params(&params.0)).await
    }

    #[doc = include_str!("../../content/tools/bt_remove_node.md")]
    #[tool(annotations(destructive_hint = true))]
    async fn bt_remove_node(&self, params: Parameters<BtRemoveNodeParams>) -> CallToolResult {
        self.tree_call("bt.remove_node", to_params(&params.0)).await
    }

    #[doc = include_str!("../../content/tools/bt_move_node.md")]
    #[tool]
    async fn bt_move_node(&self, params: Parameters<BtMoveNodeParams>) -> CallToolResult {
        self.tree_call("bt.move_node", to_params(&params.0)).await
    }

    #[doc = include_str!("../../content/tools/bt_set_params.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn bt_set_params(&self, params: Parameters<BtSetParamsParams>) -> CallToolResult {
        if params.0.params.is_empty() {
            return invalid_input(
                "bt_set_params: `params` is empty; give at least one parameter, e.g. \
                 {\"seconds\": 2} (null removes one).",
            );
        }
        self.tree_call("bt.set_params", to_params(&params.0)).await
    }

    #[doc = include_str!("../../content/tools/bt_status.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn bt_status(&self, params: Parameters<EntityParams>) -> CallToolResult {
        let args = to_params(&params.0);
        let value = match self.engine.call("bt.status", Some(args.clone())).await {
            Ok(v) => v,
            Err(e) => return error_result(&e),
        };
        let Ok(reply) = serde_json::from_value::<BtStatusReply>(value.clone()) else {
            return json_result(&value);
        };
        // Node names make the outline readable; without them it still works.
        let names = self
            .engine
            .call("bt.get", Some(args))
            .await
            .ok()
            .and_then(|v| serde_json::from_value::<BtTreeView>(v).ok())
            .map(|view| labels(&view))
            .unwrap_or_default();
        CallToolResult::success(vec![ContentBlock::text(format_status(&reply, &names))])
    }
}
