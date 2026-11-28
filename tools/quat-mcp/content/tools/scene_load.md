Replace the current scene with one loaded from a project-relative JSON path; returns `{path}`. Unsaved changes are lost.

The user is asked to confirm first (the client shows them a question). If the client cannot ask, the call is refused with the question: ask the user yourself and call again with `confirm: true` only if they agree.
