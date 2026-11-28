Delete a Lua script file from `assets/scripts/`; returns `{path, deleted, entities_still_using}`. Entities keep their `script` component (pointing at the missing file) — remove it with `scene_remove_components` (`["script"]`) or point it at another script. Running instances keep their loaded code until play stops.

The user is asked to confirm first (the client shows them a question). If the client cannot ask, the call is refused with the question: ask the user yourself and call again with `confirm: true` only if they agree.
