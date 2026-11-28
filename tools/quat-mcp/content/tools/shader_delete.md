Delete an authored material shader: its source file and compiled programs. Every entity whose `primitive.shader` (or `instanced_mesh` primitive) named it is reset to the built-in `standard` material; the reply says how many (`entities_reset`). The built-in material cannot be deleted. This cannot be undone — read the source with `shader_get` first if it may be needed again.

The user is asked to confirm first (the client shows them a question). If the client cannot ask, the call is refused with the question: ask the user yourself and call again with `confirm: true` only if they agree.
