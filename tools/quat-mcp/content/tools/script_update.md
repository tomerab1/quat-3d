Replace an existing Lua script with a new full source (compiled first; a syntax error names the line and nothing is written) and return `{path, bytes, entities, running?, status?}`.

While the game plays the change hot-reloads at once: every entity using the script gets `on_destroy`, a fresh `self` and `on_start` with the new code. If the new code fails while loading (an error in its top-level code), the previous version keeps running and `status` carries the error. Errors raised later by callbacks appear in `debug_log` (category `script`) and in `script_list` — check them after updating.

Read the current file with `script_get` first; this replaces the whole file.
