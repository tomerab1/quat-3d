Delete every entity in the scene (lights are kept unless `keep_lights=false`); returns `{destroyed: n}`. Use to start a world from scratch.

The user is asked to confirm first (the client shows them a question). If the client cannot ask, the call is refused with the question: ask the user yourself and call again with `confirm: true` only if they agree.
