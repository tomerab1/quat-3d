//! Readable renderings of the engine's behaviour-tree replies: an indented
//! outline (ids, kinds, leaf parameters, issues) the model can plan edits
//! from, followed by the tree JSON it can send back to `bt_set`.

use std::collections::{HashMap, HashSet};
use std::fmt::Write as _;

use serde_json::{Value, json};

use crate::params::{BtNodeView, BtStatusReply, BtTreeView};

const INDENT: &str = "  ";

/// A JSON number as written by hand (6 rather than 6.0).
fn value_text(v: &Value) -> String {
    v.as_f64().map_or_else(|| v.to_string(), |f| format!("{f}"))
}

fn node_label(n: &BtNodeView) -> String {
    let mut label = match &n.leaf {
        Some(leaf) => leaf.clone(),
        None => n.kind.clone(),
    };
    if let Some(params) = &n.params {
        for (name, value) in params {
            let _ = write!(label, " {name}={}", value_text(value));
        }
    }
    label
}

/// Depth of every node reachable from `root` (pre-order walk; cycle-safe).
fn depths(nodes: &[BtNodeView], root: Option<u32>) -> HashMap<u32, usize> {
    let by_id: HashMap<u32, &BtNodeView> = nodes.iter().map(|n| (n.id, n)).collect();
    let mut out = HashMap::new();
    let mut stack: Vec<(u32, usize)> = root.into_iter().map(|r| (r, 0)).collect();
    while let Some((id, depth)) = stack.pop() {
        if out.insert(id, depth).is_some() {
            continue;
        }
        if let Some(n) = by_id.get(&id) {
            stack.extend(n.children.iter().rev().map(|c| (*c, depth + 1)));
        }
    }
    out
}

/// The tree view as an outline, its issues, and the tree JSON.
pub(super) fn format_tree_view(view: &BtTreeView) -> String {
    let mut text = String::new();
    if let Some(node) = view.node {
        let _ = writeln!(text, "Added node #{node}.");
    }
    let _ = writeln!(
        text,
        "Behaviour tree of entity {} ({}, {}{}):",
        view.entity,
        if view.enabled { "enabled" } else { "disabled" },
        if view.compiled {
            "compiled"
        } else {
            "compiles on its next tick in Play"
        },
        if view.parse_failed {
            ", runtime parse failed"
        } else {
            ""
        }
    );
    if view.nodes.is_empty() {
        match &view.source {
            Some(source) => {
                let _ = writeln!(text, "(the stored text is not a tree: {source})");
            }
            None => text.push_str("(no nodes)\n"),
        }
    }
    let depth = depths(&view.nodes, view.root);
    for n in &view.nodes {
        let level = depth.get(&n.id).copied();
        let _ = writeln!(
            text,
            "{}#{} {}{}",
            INDENT.repeat(level.unwrap_or(0)),
            n.id,
            node_label(n),
            if level.is_none() {
                " (not connected)"
            } else {
                ""
            }
        );
    }
    if view.issues.is_empty() {
        text.push_str("No issues.\n");
    } else {
        text.push_str("Issues:\n");
        for issue in &view.issues {
            match (issue.node, &issue.path) {
                (0, _) => {
                    let _ = writeln!(text, "- {}", issue.message);
                }
                (id, Some(path)) => {
                    let _ = writeln!(text, "- node #{id} at {path}: {}", issue.message);
                }
                (id, None) => {
                    let _ = writeln!(text, "- node #{id}: {}", issue.message);
                }
            }
        }
    }
    if !view.tree.is_null() {
        let _ = write!(
            text,
            "Tree JSON: {}",
            json!({"root": view.root, "tree": view.tree})
        );
    }
    text
}

/// The status reply as an outline; `labels` (id -> label, from `bt.get`)
/// names the nodes when available.
pub(super) fn format_status(reply: &BtStatusReply, labels: &HashMap<u32, String>) -> String {
    let mut text = String::new();
    let play = match reply.playing {
        Some(true) => "playing",
        Some(false) => "not playing",
        None => "play state unknown",
    };
    let _ = write!(
        text,
        "Entity {}: {play}, tree {}, tick {}",
        reply.entity,
        if reply.enabled { "enabled" } else { "disabled" },
        reply.tick
    );
    if let Some(root) = &reply.root_status {
        let _ = write!(text, ", root {root}");
    }
    text.push_str(".\n");
    if reply.nodes.is_empty() {
        text.push_str(
            "No node results: the tree has not ticked since it was last changed (trees run \
             only during Play; start it with sim_play).\n",
        );
        return text;
    }
    let mut seen = HashSet::new();
    for n in &reply.nodes {
        let depth = n
            .path
            .as_deref()
            .map_or(0, |p| p.matches(".children[").count());
        let id = n.id.map_or_else(|| "?".to_owned(), |id| format!("#{id}"));
        let label =
            n.id.filter(|id| seen.insert(*id))
                .and_then(|id| labels.get(&id))
                .map_or_else(String::new, |l| format!(" {l}"));
        let _ = writeln!(text, "{}{id}{label}: {}", INDENT.repeat(depth), n.status);
    }
    text
}

/// Labels for `format_status` from a tree view.
pub(super) fn labels(view: &BtTreeView) -> HashMap<u32, String> {
    view.nodes.iter().map(|n| (n.id, node_label(n))).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::params::BtNodeStatus;

    fn view() -> BtTreeView {
        serde_json::from_value(json!({
            "entity": 12, "enabled": true, "compiled": false, "parse_failed": false,
            "root": 1,
            "nodes": [
                {"id": 1, "kind": "selector", "children": [2, 3], "path": "root"},
                {"id": 2, "kind": "leaf", "leaf": "near_camera", "params": {"range": 6.0},
                 "children": [], "path": "root.children[0]"},
                {"id": 3, "kind": "leaf", "leaf": "move_to",
                 "params": {"x": 4.0, "z": -3.5}, "children": [], "path": "root.children[1]"}
            ],
            "issues": [],
            "tree": {"type": "selector", "children": []}
        }))
        .unwrap()
    }

    #[test]
    fn tree_views_render_as_an_indented_outline() {
        let text = format_tree_view(&view());
        assert!(
            text.starts_with("Behaviour tree of entity 12 (enabled, compiles on its next tick")
        );
        assert!(
            text.contains("\n#1 selector\n  #2 near_camera range=6\n  #3 move_to x=4 z=-3.5\n")
        );
        assert!(text.contains("No issues."));
        assert!(
            text.ends_with(r#"Tree JSON: {"root":1,"tree":{"children":[],"type":"selector"}}"#)
        );
    }

    #[test]
    fn issues_name_the_node_and_its_path() {
        let mut v = view();
        v.issues = serde_json::from_value(json!([
            {"node": 3, "path": "root.children[1]", "message": "move_to.speed: expected a number"},
            {"node": 0, "message": "the tree is empty"}
        ]))
        .unwrap();
        v.node = Some(3);
        let text = format_tree_view(&v);
        assert!(text.starts_with("Added node #3.\n"));
        assert!(text.contains("- node #3 at root.children[1]: move_to.speed: expected a number\n"));
        assert!(text.contains("- the tree is empty\n"));
    }

    #[test]
    fn status_lines_follow_the_tree() {
        let reply = BtStatusReply {
            entity: json!(12),
            playing: Some(true),
            enabled: true,
            compiled: true,
            tick: 40,
            root_status: Some("running".into()),
            nodes: vec![
                BtNodeStatus {
                    id: Some(1),
                    path: Some("root".into()),
                    status: "running".into(),
                },
                BtNodeStatus {
                    id: Some(2),
                    path: Some("root.children[0]".into()),
                    status: "failure".into(),
                },
            ],
        };
        let text = format_status(&reply, &labels(&view()));
        assert!(text.starts_with("Entity 12: playing, tree enabled, tick 40, root running.\n"));
        assert!(text.contains("#1 selector: running\n  #2 near_camera range=6: failure\n"));

        let idle = BtStatusReply {
            nodes: vec![],
            root_status: None,
            ..reply
        };
        assert!(format_status(&idle, &HashMap::new()).contains("start it with sim_play"));
    }
}
