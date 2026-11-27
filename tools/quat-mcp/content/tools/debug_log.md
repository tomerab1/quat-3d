Read the running engine's recent log: engine messages, warnings, errors and Vulkan validation messages (category `vulkan`). Start here when something looks wrong.

Returns the lines as text, one per line (`#seq t=<seconds> <level> [<category>] <message>`), then a JSON footer `{cursor, more, missed}`. Without `since_seq` you get the newest `limit` lines (default 100). To follow the log, pass the returned `cursor` as `since_seq` next time: you then get only lines written since. `more: true` means `limit` cut the read short (call again from `cursor`); `missed` > 0 means lines were overwritten before you read them (the engine keeps the last 4096).

Narrow with `level` (the minimum: `trace` < `debug` < `info` < `warn` < `error`), `category` (exact, e.g. `vulkan`, `nav`, `remote`, `editor`, `stderr`) and `contains` (case-sensitive substring). Example: `{"level": "warn"}` for every warning and error.
