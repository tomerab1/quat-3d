The vocabulary of behaviour trees: every node kind and every leaf the running engine knows, with each leaf's parameters (type, default, accepted range, whether it may be omitted, what it means). Call it before writing a tree — it is the authority, so never guess leaf or parameter names.

Returns `{kinds: [{name, min_children, max_children, help}], leaves: [{name, help, params: [{name, type, default, min?, max?, optional, help}]}]}`. `max_children` null means any number.

Kinds: composites run their children in order — `sequence` (until one fails), `selector` (until one does not fail), their `reactive_` variants (re-check earlier children every tick, so a condition that changes aborts or pre-empts the running branch), and `inverter` (one child, swaps success and failure). Leaves (`kind: "leaf"`) are actions (`move_to`, `wait`) and conditions (`near_point`, `near_camera`).
