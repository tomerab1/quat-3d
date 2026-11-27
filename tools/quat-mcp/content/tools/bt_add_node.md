Add one node to an entity's behaviour tree by id (see `bt_get`) — for small changes without resending the whole tree. Returns the tree like `bt_get`, starting with `Added node #N.`

- A leaf: `{"entity": "Guard", "parent": 6, "index": 1, "kind": "leaf", "leaf": "wait", "params": {"seconds": 2}}` inserts it as the second child of node 6 (`index` counts from 0; omit it to append).
- A composite needs its children at once (an empty composite is invalid): give new subtrees as tree JSON and/or ids of existing nodes to move under it. Wrapping node 7 in an inverter, in its place: `{"entity": "Guard", "parent": 6, "index": 0, "kind": "inverter", "children": [7]}`. A new branch: `{"parent": 1, "kind": "sequence", "children": [{"type": "leaf", "name": "near_point", "x": 0, "y": 0, "z": 5, "range": 2}, {"type": "leaf", "name": "wait", "seconds": 1}]}`.
- `parent` null makes the new node the root: allowed for an empty tree, or when `children` includes the current root.

`index` is the position after the adopted children have been taken out of their old places. Nothing changes if the result would be invalid; the error names the offending field or node.
