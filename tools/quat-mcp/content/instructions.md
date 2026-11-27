quat-mcp drives a running quat-3d game engine (Vulkan renderer, Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take screenshots to see the result.

Good workflow: `scene_components` and `scene_list` to inspect, plan with coordinates in metres (Y up), build with `scene_batch` (`"@N"` refers to the id created by command N of the batch), then `screenshot` (with a `camera` to frame what you built) and iterate with `scene_update`.

World building:
- Terrain: after creating or reshaping a terrain (new `seed`, `height`, `tile_size`, ...), wait until `terrain_status` reports `generating: false` before placing things or taking screenshots. Shading edits such as `snowline` apply at once and need no wait.
- Models: `asset_list_models` lists what the project has; `asset_search` + `asset_fetch` download free CC0 models from Poly Haven (trees, rocks, props) into the project. Mind `polycount` when you need many copies; use an `instanced_mesh` (plus `scatter`) for forests and rock fields instead of one entity per copy.
- Placement: `scene_get` reports `world_bounds` / `local_bounds` (size and origin of any model); `world_place_on_ground` sets an entity down on the terrain or on colliders below it, optionally tilted to the slope. Do not guess sink depths.
- Looking: the user may be using the viewport. Prefer `screenshot` with a `camera` (a position and `look_at`, or `frame` an entity) for verification shots, which leaves their view alone; use `camera_set` only to change what they see. The camera's clip planes follow the scene (distant streamed terrain stays visible), so leave `near` / `far` alone unless a shot needs them.

To change how the scene looks, tune the renderer (`render_settings_get` / `render_settings_set` / `render_settings_schema`: exposure, bloom, anti-aliasing, shadows, sky light, clouds) and per-entity materials (`material_get` / `material_set`), checking each change with `screenshot`; the `tune_look` prompt walks through it.

NPC behaviour is a behaviour tree per entity: `bt_leaves` lists the node kinds and leaves, `bt_set` writes a whole tree, `bt_get` shows it with stable node ids for the incremental `bt_add_node` / `bt_move_node` / `bt_remove_node` / `bt_set_params`, and `bt_status` shows what each node did on the last tick during Play.

Gameplay is written as Lua scripts: read the `quat://docs/scripting` resource, write files with `script_create` / `script_update` (compiled first; errors name the line), attach them with the `script` component, run with `sim_play` and read `debug_log` (category `script`). The `write_game_script` prompt walks through it.

Custom looks: write a material shader (a Slang module) with `shader_template` → `shader_create`, assign it with `scene_update` (`primitive.shader`), screenshot and refine with `shader_update`; compile errors come back with file, line and column.

Engine errors name the offending field and what was expected — fix the call instead of repeating it. The `build_world`, `inspect_scene`, `debug_session`, `design_behavior`, `tune_look`, `write_game_script` and `write_material_shader` prompts describe the workflows in detail.
