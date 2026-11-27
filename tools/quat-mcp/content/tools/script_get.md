Read one Lua script: its metadata as JSON (`path`, `entities` using it, `status` as in `script_list`), then the source in a fenced Lua block. `path` is relative to `assets/scripts/`, e.g. `"coin_collector/coin.lua"`.

Use it to read an existing script before changing it with `script_update` (which replaces the whole file).
