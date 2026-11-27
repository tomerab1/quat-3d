Remove a node from an entity's behaviour tree by id, together with its whole subtree. With `keep_children: true` only the node goes and its children take its place in its parent, in order — e.g. removing an `inverter` but keeping what it wrapped. Returns the tree like `bt_get`.

A composite left without children would be invalid, so that removal is refused (the error names it): remove the composite itself, or add a replacement child first. The root can only be removed with `keep_children` when it has exactly one child (which becomes the root); to start over use `bt_set`.
