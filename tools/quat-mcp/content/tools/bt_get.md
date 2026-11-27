Show an entity's behaviour tree — the same graph the editor's Behaviour panel shows. Returns an indented outline with every node's id (`#7`), kind or leaf name and parameters, then any issues that keep the stored tree from running, then the tree JSON (without the editor's layout) that you can modify and send back to `bt_set`.

Node ids are stable: edits keep them, and new nodes get new ones. Use them with `bt_add_node`, `bt_remove_node`, `bt_move_node`, `bt_set_params` and to read `bt_status`. An entity without a tree is a `not_found` error — create one with `bt_set`.
