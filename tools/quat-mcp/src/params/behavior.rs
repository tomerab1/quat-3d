//! Inputs of the behaviour-tree tools (`bt_*`, docs/remote-control.md
//! "Behaviour-tree authoring"), and the shapes of the engine's tree view and
//! status replies.

use std::collections::BTreeMap;

use schemars::JsonSchema;
use serde::de::Error as _;
use serde::{Deserialize, Deserializer, Serialize};
use serde_json::{Map, Value};

use super::{EntityRef, object_or_json_string};

/// A behaviour-tree node as the runtime's JSON: `{"type": <kind>, "children":
/// [...]}` for composites, `{"type": "leaf", "name": <leaf>, <param>: <number>}`
/// for leaves.
pub type TreeNode = Map<String, Value>;

/// Like `object_or_json_string`, for a required field.
fn required_tree<'de, D>(d: D) -> Result<TreeNode, D::Error>
where
    D: Deserializer<'de>,
{
    object_or_json_string(d)?.ok_or_else(|| {
        D::Error::custom("expected the behaviour tree as an object, e.g. {\"type\": \"sequence\"}")
    })
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BtSetParams {
    /// The entity whose tree to replace: integer id (preferred) or exact unique
    /// name. Its `behavior` component is created if it has none.
    pub entity: EntityRef,
    /// The whole tree, e.g. {"type": "sequence", "children": [{"type": "leaf",
    /// "name": "wait", "seconds": 2}]}. Node kinds and leaves: `bt_leaves`.
    #[serde(deserialize_with = "required_tree")]
    pub tree: TreeNode,
    /// Whether the tree runs during Play (default: unchanged; true for a new tree).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
}

/// A child of a new composite: an existing node to move under it, or a new
/// subtree.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum BtChild {
    /// Id of an existing node (it moves under the new node with its subtree).
    Existing(u32),
    /// A new subtree as tree JSON, e.g. {"type": "leaf", "name": "wait"}.
    New(TreeNode),
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BtAddNodeParams {
    /// The entity whose tree to edit: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Id of the composite to add the node under. Null (or omitted) makes the
    /// new node the root: allowed for an empty tree, or when `children` adopts
    /// the current root.
    #[serde(default)]
    pub parent: Option<u32>,
    /// Position among the parent's children, 0 = first (default: last).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub index: Option<u32>,
    /// Node kind as listed by `bt_leaves`, e.g. "sequence", "selector",
    /// "inverter" or "leaf".
    pub kind: String,
    /// For kind "leaf": the leaf's name, e.g. "`move_to`" (see `bt_leaves`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub leaf: Option<String>,
    /// For kind "leaf": parameter values, e.g. {"x": 4, "z": -3}. Omitted
    /// parameters take their defaults.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub params: Option<BTreeMap<String, f64>>,
    /// For composites (at least one): new subtrees as tree JSON and/or ids of
    /// existing nodes to move under the new node, in tick order.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub children: Option<Vec<BtChild>>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BtRemoveNodeParams {
    /// The entity whose tree to edit: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Id of the node to remove (from `bt_get`).
    pub node: u32,
    /// Keep the node's children: they take its place in its parent (default
    /// false: the whole subtree is removed).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub keep_children: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BtMoveNodeParams {
    /// The entity whose tree to edit: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Id of the node to move (it moves with its subtree).
    pub node: u32,
    /// Id of the composite to move it under (may be its current parent, to re-order).
    pub parent: u32,
    /// Position among the new parent's other children, 0 = first (default: last).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub index: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BtSetParamsParams {
    /// The entity whose tree to edit: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Id of the leaf node (from `bt_get`).
    pub node: u32,
    /// Parameters to set, e.g. {"seconds": 2}; null removes one (back to its
    /// default — for `move_to`, a null `y` snaps the target to the ground).
    pub params: BTreeMap<String, Option<f64>>,
}

/// One node of the engine's tree view.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BtNodeView {
    pub id: u32,
    pub kind: String,
    #[serde(default)]
    pub leaf: Option<String>,
    #[serde(default)]
    pub params: Option<Map<String, Value>>,
    #[serde(default)]
    pub children: Vec<u32>,
    #[serde(default)]
    pub path: Option<String>,
}

/// A problem that keeps a tree from running.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BtIssue {
    /// The node it concerns (0: the tree as a whole).
    pub node: u32,
    #[serde(default)]
    pub path: Option<String>,
    pub message: String,
}

/// The engine's tree view (`bt.get`, `bt.set` and the edit methods).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BtTreeView {
    pub entity: Value,
    pub enabled: bool,
    pub compiled: bool,
    #[serde(default)]
    pub parse_failed: bool,
    pub root: Option<u32>,
    pub nodes: Vec<BtNodeView>,
    pub issues: Vec<BtIssue>,
    pub tree: Value,
    /// The stored text, when it is not JSON.
    #[serde(default)]
    pub source: Option<String>,
    /// `bt.add_node`: the new node's id.
    #[serde(default)]
    pub node: Option<u32>,
}

/// One node's result on the tree's last tick.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BtNodeStatus {
    pub id: Option<u32>,
    pub path: Option<String>,
    pub status: String,
}

/// The engine's `bt.status` reply.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BtStatusReply {
    pub entity: Value,
    pub playing: Option<bool>,
    pub enabled: bool,
    pub compiled: bool,
    pub tick: u64,
    pub root_status: Option<String>,
    pub nodes: Vec<BtNodeStatus>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn set_accepts_a_tree_object_or_its_text() {
        let p: BtSetParams = serde_json::from_value(
            json!({"entity": 3, "tree": "{\"type\": \"leaf\", \"name\": \"wait\"}"}),
        )
        .unwrap();
        assert_eq!(p.tree["name"], "wait");
        assert!(serde_json::from_value::<BtSetParams>(json!({"entity": 3})).is_err());
        assert!(
            serde_json::from_value::<BtSetParams>(json!({"entity": 3, "tree": "[1]"})).is_err()
        );
    }

    #[test]
    fn add_node_sends_a_null_parent_and_mixed_children() {
        let p: BtAddNodeParams = serde_json::from_value(json!({
            "entity": "Guard", "kind": "inverter",
            "children": [7, {"type": "leaf", "name": "wait"}]
        }))
        .unwrap();
        assert_eq!(
            p.children.as_deref().unwrap()[0],
            BtChild::Existing(7),
            "ids are existing nodes"
        );
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"entity": "Guard", "parent": null, "kind": "inverter",
                   "children": [7, {"type": "leaf", "name": "wait"}]})
        );
    }

    #[test]
    fn set_params_serializes_null_removals() {
        let p: BtSetParamsParams = serde_json::from_value(
            json!({"entity": 1, "node": 7, "params": {"y": null, "speed": 4}}),
        )
        .unwrap();
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"entity": 1, "node": 7, "params": {"speed": 4.0, "y": null}})
        );
        assert!(
            serde_json::from_value::<BtSetParamsParams>(
                json!({"entity": 1, "node": 7, "params": {"speed": "fast"}})
            )
            .is_err()
        );
    }
}
