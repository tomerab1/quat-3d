---
name: design-npc-behaviour
description: Design NPC behaviour in a running quat-3d game as behaviour trees - patrols, guards, followers, alert-and-react priorities - built from the engine's own leaf vocabulary, validated on write, then played and observed node by node with navigation checks. Use when asked to make an NPC or creature walk, patrol, wait, chase, flee or react in quat-3d.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "bt_leaves bt_get bt_set bt_add_node bt_move_node bt_remove_node bt_set_params bt_status scene_list scene_get scene_update nav_bake sim_play sim_stop debug_pause debug_step debug_watch debug_nav_path debug_log screenshot"
---

# Design NPC behaviour

Each NPC carries a behaviour tree (the `behavior` component). Learn the
vocabulary from the engine, plan the priorities, write the tree, play it and
watch what each node did.

## Workflow

1. **Learn the vocabulary.** `bt_leaves` returns every node kind and leaf
   with its parameters, defaults and ranges. It is the authority: use only
   those names. Composites run their children in order: `sequence` (until
   one fails), `selector` (until one does not fail), their `reactive_`
   variants (re-check earlier children every tick, so a condition that
   changes pre-empts the running branch), and `inverter` (one child, swaps
   success and failure).
2. **Find the NPC.** `scene_list` and `scene_get`. A tree that uses
   `move_to` needs a `nav_agent` component on the entity: add it with
   `scene_update`. If the entity already has a tree, read it with `bt_get`.
3. **Plan before writing.** Describe the behaviour as priorities, highest
   first, and map them onto nodes:
   - a `reactive_selector` at the top, so an alert branch (condition, then
     reaction) pre-empts a patrol branch below it;
   - a `sequence` for steps in order (walk, wait, walk on);
   - an `inverter` around a condition for "not near";
   - conditions (`near_point`, `near_camera`) first in their branch.

   Take coordinates from the scene (`world_position` of landmarks in
   `scene_list`).
4. **Build it.** `bt_set` writes the whole tree:

   ```json
   {"entity": "Guard", "tree": {"type": "reactive_selector", "children": [
     {"type": "sequence", "children": [
       {"type": "leaf", "name": "near_camera", "range": 6},
       {"type": "leaf", "name": "wait", "seconds": 1}]},
     {"type": "sequence", "children": [
       {"type": "leaf", "name": "move_to", "x": 4, "z": -3, "speed": 2.5},
       {"type": "leaf", "name": "wait", "seconds": 3},
       {"type": "leaf", "name": "move_to", "x": -4, "z": -3, "speed": 2.5}]}]}}
   ```

   Change an existing tree incrementally with `bt_add_node`,
   `bt_move_node`, `bt_remove_node` and `bt_set_params`, using the node ids
   from `bt_get`. Nothing is stored when the tree is invalid: the error
   names each problem with the node's id and path; fix exactly those.
5. **Run it.** `sim_play` (a navmesh is baked on Play when there is none),
   let it run a moment or `debug_pause` and `debug_step` some frames, then:
   - `bt_status`: which node is running, which condition failed, which
     branch was never reached;
   - `debug_watch` the entity: its position and `nav_agent_state`
     (`failed`, `path_partial`, `stuck_s`) when a `move_to` does not arrive;
   - `debug_nav_path` from the NPC's position to its target: is there a
     path, or only a `partial` one?
   - `screenshot` to see where it is; `debug_log` with
     `{"category": "bt"}` or `{"category": "nav"}` for tree and navigation
     messages.
6. **Iterate.** `sim_stop` first: Stop restores the scene as it was when
   Play started, so tree edits made while playing are undone. Experimenting
   during Play is fine; re-apply what worked after stopping. Tune with
   `bt_set_params` (speeds, waits, ranges) or restructure, then play again.
7. **Finish** stopped, and summarise the final tree (from `bt_get`) and how
   it meets the goal.

## Verify

- `bt_status` during Play shows the intended branch `running` in each
  situation you care about (for example, near the camera and away from it).
- `debug_watch` shows the NPC's position reaching each `move_to` target
  (`arrived`), with `failed` false and `stuck_s` near 0.

## Pitfalls

- Trees tick only during Play; right after a change the tree has not ticked
  yet, so `bt_status` has no results until a frame has run.
- A `move_to` target off the navmesh, on a roof or across a gap gives a
  `partial` path: the NPC stops at the closest reachable point. Check with
  `debug_nav_path` and pick reachable targets.
- After moving static geometry, `nav_bake` again; Play only bakes when no
  navmesh exists.
- Removing a leaf's `y` with `bt_set_params` (`{"y": null}`) makes a
  `move_to` target snap to the ground.
- A simple loop between fixed points may not need a tree at all: the
  `patrol` component walks its `points` with the entity's `nav_agent`.
