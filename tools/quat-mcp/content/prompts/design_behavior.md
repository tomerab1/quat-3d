Design a behaviour tree in the running quat-3d engine using the quat-mcp tools. The goal: {{ goal }}
{%- if entity %}

Give the behaviour to this entity: {{ entity }}
{%- endif %}

Work in this order:

1. **Learn the vocabulary.** Call `bt_leaves`: the node kinds and every leaf with its parameters, defaults and ranges. Use only those names. Find the NPC with `scene_list` / `scene_get`; if the tree uses `move_to`, the entity needs a `nav_agent` component (add it with `scene_update`). If it already has a tree, read it with `bt_get`.

2. **Plan the tree before writing it.** Describe the behaviour as priorities, highest first, and map them onto nodes: a `reactive_selector` at the top re-checks higher-priority branches every tick, so an alert branch (condition, then reaction) pre-empts a patrol branch; a `sequence` runs steps in order (walk, wait, walk on); an `inverter` turns a condition around ("not near"). Conditions (`near_point`, `near_camera`) go first in their branch. Pick coordinates in metres (Y up) from the scene, e.g. `world_position`s of landmarks from `scene_list`.

3. **Build it.** Write the whole tree with `bt_set`, or change an existing one incrementally with `bt_add_node`, `bt_move_node`, `bt_remove_node` and `bt_set_params` using the node ids from `bt_get`. Nothing is stored when the tree is invalid: the error names each problem with the node's id and path — fix exactly those and call again.

4. **Run it.** `sim_play`, let it run a moment (or `debug_pause` and `debug_step` some frames), then:
   - `bt_status`: which node is running, which condition failed, which branch was never reached;
   - `debug_watch` the entity: its position, and `nav_agent_state` (`failed`, `path_partial`, `stuck_s`) when a `move_to` does not arrive;
   - `screenshot` to see where it is, `debug_log` for `[bt]` / `[nav]` messages.

5. **Iterate.** Compare what happened with the goal, then `sim_stop` — Stop restores the scene as it was when Play started, so changes made while playing are undone (experimenting during Play is fine; re-apply what worked after stopping). Tune parameters with `bt_set_params` (speeds, waits, ranges) or restructure the tree, then play and observe again. Finish stopped, and summarise the final tree (from `bt_get`) and how it meets the goal.
