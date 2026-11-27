Give an entity a whole behaviour tree (replacing any it has; the `behavior` component is created if missing). The tree is the runtime's JSON: composites are `{"type": <kind>, "children": [...]}`, leaves are `{"type": "leaf", "name": <leaf>, <param>: <number>, ...}` — `bt_leaves` lists the kinds, leaves and parameters. Example, a guard that walks to a post and waits there unless the viewer comes close:

```json
{"entity": "Guard", "tree": {"type": "reactive_selector", "children": [
  {"type": "sequence", "children": [
    {"type": "leaf", "name": "near_camera", "range": 6},
    {"type": "leaf", "name": "wait", "seconds": 1}]},
  {"type": "sequence", "children": [
    {"type": "leaf", "name": "move_to", "x": 4, "z": -3, "speed": 2.5},
    {"type": "leaf", "name": "wait", "seconds": 3}]}]}}
```

The whole tree is validated first and nothing changes if anything is wrong: the error lists every problem with the node's id and path, e.g. `node 6 at root.children[1].children[0]: move_to.speed: expected a number in [0, 1000]` — fix those and call again. On success it returns the tree like `bt_get`. A tree of the same shape as the current one (only parameters changed) keeps its node ids and editor layout; otherwise nodes are numbered 1..N in depth-first order. The tree recompiles and restarts from its root on its next tick; trees only tick during Play (`sim_play`), and `sim_stop` restores the scene Play started from, so changes made while playing are undone — edit before playing to keep them. `move_to` needs a `nav_agent` component on the entity and a navmesh (Play bakes one when there is none; `nav_bake` rebakes after the level changes).
