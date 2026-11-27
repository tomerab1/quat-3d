Change a leaf's parameters by node id — the quickest way to tune a behaviour (a walk speed, a wait, a detection range). Only the given parameters change: `{"entity": "Guard", "node": 7, "params": {"speed": 4}}`. `null` removes a parameter so its default applies again; for `move_to`, removing `y` makes the target snap to the ground. Returns the tree like `bt_get`.

Values are checked against the leaf's ranges (`bt_leaves`); an unknown name or out-of-range value is refused with the accepted ones and nothing changes. The tree restarts from its root on its next tick.
