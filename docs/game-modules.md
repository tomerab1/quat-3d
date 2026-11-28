# Native game modules (G17.1)

A game that needs native code adds a game module: a static C++ library with
one registration function. The engine links the module into the game's own
player, shipping player and editor, and calls that function at boot. Through
an `engine::game::Registrar` the module declares:

- its components;
- the systems that run every frame;
- its remote methods;
- its Lua modules;
- its input actions.

Games made of data and Lua only need none of this. They run in the stock
`quat-player` (see [`game-packaging.md`](game-packaging.md)).

The design is in [`design/cozy-builder.md` §3.6](design/cozy-builder.md#36-game-modules-g17-on-top-of-p4-and-g7).
[`games/coin-collector/`](../games/coin-collector) (in this repository) and
[`examples/sdk-consumer/`](../examples/sdk-consumer) (built only against an
installed SDK) are working examples.

## Building

```cmake
find_package(quat3d 0.1 CONFIG REQUIRED)

quat_add_game_module(my_game_module          # a static library on quat3d::engine
    SOURCES module/register.cpp module/walls.cpp
    REGISTER my_game::register_module         # engine::game::RegisterFn
    NAME my_game)                             # game.json "modules" name (default:
                                              # the target without "_module")
quat_add_game(my-game
    PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}   # holds game.json
    MODULES my_game_module)
```

`quat_add_game(... MODULES)` makes three executables. Each one has a
generated `main()` that calls every module's `REGISTER` function directly.
There are no plugin DLLs and no static-initialiser registration, because a
linker drops unreferenced objects from static libraries.

| Executable | Links | What it is |
|---|---|---|
| `my-game` | `quat3d::player` | the player (`engine::game::run_player`) |
| `my-game-shipping` | `quat3d::player_shipping` | the same player with remote control compiled out. `quat-pack --config shipping --player my-game` ships it |
| `my-game-editor` | `quat3d::player_editor` | the engine editor with the game's modules (only for SDKs built with the editor) |

`PROJECT_DIR` is the project the executables open when `QUAT_GAME` is not
set. Distribution builds bake no path into the executable: set
`QUAT_GAME_PACKAGE_BUILD`, which `ENGINE_PACKAGE_BUILD` sets in the engine's
own tree. A packaged game then finds `game.json` next to its executable.

`game.json` format 2 lists the modules the game needs. A player that was not
built with all of them refuses to run the game and names the missing one:

```json
{ "format": 2, "name": "My Game", "id": "com.example.my-game", "version": "0.1.0",
  "start_scene": "assets/scenes/main.json",
  "modules": ["my_game"], "mcp": "mcp", "engine": ">=0.1,<0.2" }
```

`engine` is the range of engine versions the game was built for. The range
is a list of comparators separated by commas (`>=`, `>`, `<=`, `<`, `=`). A
project whose range excludes the running engine fails to load with "this game
needs engine …". `mcp` is the folder of game MCP tools and skills (G17.2).

## Registering

```cpp
#include "engine/game/module.hpp"

namespace my_game {

struct Lantern { float range = 6.0F; glm::vec3 tint{1.0F}; bool lit = true; };

void flicker(engine::game::SystemContext& ctx, float dt) {
    for (auto [e, lantern] : ctx.registry().view<Lantern>().each()) { /* ... */ }
}

std::expected<void, engine::core::Error> register_module(engine::game::Registrar& r) {
    r.component<Lantern>("my_lantern", "A lantern on a post",
        engine::game::ComponentFields<Lantern>()
            .field("range", &Lantern::range, {.min = 0.5, .max = 50.0, .description = "Light radius, m"})
            .color("tint", &Lantern::tint)
            .field("lit", &Lantern::lit));
    r.system({.name = "my_game.flicker", .phase = engine::game::Phase::post_transform, .run = &flicker});
    r.method({.name = "lanterns.count", .summary = "How many lanterns: {} -> {count}.",
              .params_schema = {{"type", "object"}, {"additionalProperties", false}},
              .handler = [](engine::game::SystemContext& ctx, const nlohmann::json&)
                  -> std::expected<nlohmann::json, engine::core::Error> {
                  return nlohmann::json{{"count", ctx.registry().view<Lantern>().size()}};
              }});
    r.input_actions("input/my_game.json");
    return {};
}

} // namespace my_game
```

The `Registrar` never fails a call on the spot. It collects every problem,
such as a duplicate name, an unknown `after`, an engine namespace or an
unsupported schema keyword. At boot the player reports all of them together,
each with the module's name, and exits.

### Components

A field table (`engine/game/fields.hpp`) describes a component field by
field. From it the engine derives:

- the JSON codec, used by scene files, `scene.*` remote methods and Lua
  `entity:get` / `set`, with strict range checks;
- a JSON Schema, which `scene.components` reports;
- the editor's Inspector rows, including Add Component, reset, copy, paste
  and remove.

A game never includes ImGui. The supported field types are `bool`,
`std::int32_t`, `float`, `glm::vec2`, `glm::vec3`, `glm::vec4`,
`std::string`, colours (`.color()`, which also accepts `"#rrggbb"`) and named
choices (`.choice()`). A field marked `read_only` is written to JSON but never
read back from it. Prefix component names with the game's name (`cozy_wall`).

### Systems and phases

A system is a stateless function. Its state lives in components. Systems run
only in play: the player is always in play, and the editor runs them during
Play.

| Phase | Runs | Typical use |
|---|---|---|
| `input` | once per frame, after input sampling, before scripts and physics | read actions, set intents |
| `pre_physics` | once per physics slice, before the physics and character systems | drive bodies and characters |
| `post_physics` | once per physics slice, after them | react to contacts |
| `post_transform` | once per frame, after world transforms resolve | generators, scatter |
| `pre_render` | once per frame, before render collection | last-moment visual state |

Within a phase, a system runs after the systems it names in `after`. A cycle,
or an `after` naming a system that is not in the same phase, is an error.
Without physics in the frame (no physics world), `pre_physics` and
`post_physics` run once on the frame's time.

### Remote methods

`MethodDesc{name, summary, params_schema, result_schema, handler}` adds a
JSON-RPC method (see [`remote-control.md`](remote-control.md)).

- The name is `<namespace>.<verb>`. The engine's namespaces (`scene`,
  `world`, `camera`, …; see `engine_method_namespaces()`) are refused.
- Parameters are validated against `params_schema` before the handler runs.
  A violation returns `invalid_params` with the value's path, for example
  `params.size[1]: expected a number <= 64`. A handler's own error returns
  `failed`.
- `rpc.methods` lists both schemas, ready for G17.2's dynamic MCP tools.

Supported schema subset (`engine/game/json_schema.hpp`): `type`,
`properties`, `required`, `additionalProperties` (boolean), `items`,
`minItems`, `maxItems`, `enum`, `const`, `minimum`, `maximum`,
`exclusiveMinimum`, `exclusiveMaximum`, `minLength`, `maxLength` and
`anyOf`, plus the annotations. Any other keyword is refused when the method
registers, so a schema is never weaker than it reads.

### Lua modules

`lua_module({.name = "my_game", .functions = {...}})` lets scripts call
`require("my_game").fn(a, b)`.

- The arguments arrive as one JSON array, converted the way the component
  codec converts tables.
- The function returns JSON, which goes back to Lua as a value.
- An error becomes a Lua error with the message.
- Calls run under the script's instruction and memory budget.

A native Lua module shadows any `assets/scripts/<name>.lua`. `quat-pack`
does not look for a file behind `require("<name>")` when `<name>` is one of
game.json's `modules`, so name a module's Lua module after the module.

### Input actions

`input_actions("input/my_game.json")` merges a G5 action map
([`input.md`](input.md)) into the game's map. Unlike a project's own map, a
module's map may not redefine an action or context: a clash is an error.

### Services

`SystemContext::services()` holds what the application owns:

- `physics`: the running `PhysicsWorld` (null outside play), for queries,
  bodies and characters;
- `actions`: the G5 actions, evaluated this frame;
- `project_root`.

The world clock and the weather are scene components, read through the
registry. The job pool, command stack (E1), terrain edits (G19), dynamic
meshes (R23.2) and seasons (R25) join `Services` when those items land.

### Not yet

UI screens (G6.1) and save chunks (G11.1) have `Registrar::ui_screen` and
`Registrar::save_chunk` with the shapes the design names. This engine version
has neither feature, so registering one fails the boot with a message naming
the item.

## Testing a module

A module's tests link the module and GoogleTest from the SDK's `deps/`. They
load the module the way the player does, with `engine::game::ModuleSet::load`
(`engine/game/module_set.hpp`), then:

- run phases with `ModuleSet::run`;
- call methods with `ModuleSet::call`, which validates the schema too;
- call Lua functions through `lua_modules()`.

See `games/coin-collector/tests/coin_module_test.cpp`.

## Differences from the design sketch

| Design §3.6 | Here | Why |
|---|---|---|
| `component(name, ComponentFields<T>)` | `component<T>(name, summary, fields)` | listings need a summary |
| `lua_module(name, LuaBindFn)` | `lua_module(LuaModuleDesc)` (JSON in, JSON out) | keeps games free of Lua and sol2 headers, and failures run through the sandbox's checked calls |
| `ui_screen(name, path)`, `save_chunk(desc)` | the same shapes, with descriptor structs | G6.1 and G11.1 do not exist yet; registering fails with a message naming them |
| `Registrar::services()` | `SystemContext::services()` | services belong to the running world, not to boot-time registration |
| a player entry point | `engine::game::run_player` / `run_editor` (`engine/game/player.hpp`) | also callable from a hand-written `main()` |
