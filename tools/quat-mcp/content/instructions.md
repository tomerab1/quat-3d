quat-mcp drives a running quat-3d game engine (Vulkan renderer, Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take screenshots to see the result.

Good workflow: `scene_components` and `scene_list` to inspect, plan with coordinates in metres (Y up), build with `scene_batch` (`"@N"` refers to the id created by command N of the batch), then `screenshot` (with a `camera` to frame what you built) and iterate with `scene_update`.

World building:
- Terrain: after creating or reshaping a terrain (new `seed`, `height`, `tile_size`, ...), wait until `terrain_status` reports `generating: false` before placing things or taking screenshots. Shading edits such as `snowline` apply at once and need no wait.
- Models: `asset_list_models` lists what the project has; `asset_search` + `asset_fetch` download free CC0 models from Poly Haven (trees, rocks, props) into the project. Mind `polycount` when you need many copies; use an `instanced_mesh` (plus `scatter`) for forests and rock fields instead of one entity per copy.
- Placement: `scene_get` reports `world_bounds` / `local_bounds` (size and origin of any model); `world_place_on_ground` sets an entity down on the terrain or on colliders below it, optionally tilted to the slope. Do not guess sink depths.
- Looking: the user may be using the viewport. Prefer `screenshot` with a `camera` (a position and `look_at`, or `frame` an entity) for verification shots, which leaves their view alone; use `camera_set` only to change what they see. The camera's clip planes follow the scene (distant streamed terrain stays visible), so leave `near` / `far` alone unless a shot needs them.

To change how the scene looks, tune the renderer (`render_settings_get` / `render_settings_set` / `render_settings_schema`: exposure, bloom, anti-aliasing, shadows, sky light, clouds) and per-entity materials (`material_get` / `material_set`), checking each change with `screenshot`.

The weather (`weather_get` / `weather_set`) drives the wind the grass bends in, cloud cover and cloud shadows, rain or snow, wet surfaces with puddles, and snow cover; the `set-weather` workflow walks through it.

Time of day: `time_set` gives the scene a date, time and place (latitude / longitude); the sun then follows the real Sun (sunrise, sunset, night) and the clock runs with the simulation (`time_scale` speeds it up, 0 freezes it). `time_get` reports the Sun and Moon positions, sunrise / sunset and the Moon's phase; `time_skip` jumps forward or back.

NPC behaviour is a behaviour tree per entity: `bt_leaves` lists the node kinds and leaves, `bt_set` writes a whole tree, `bt_get` shows it with stable node ids for the incremental `bt_add_node` / `bt_move_node` / `bt_remove_node` / `bt_set_params`, and `bt_status` shows what each node did on the last tick during Play.

Physics runs during Play (`sim_play`). `physics_get` / `physics_set` / `physics_apply` read and push bodies, `physics_set_gravity` changes the scene's gravity, and `physics_raycast_all` / `physics_overlap` / `physics_shape_cast` query the world. `events_poll` reports which bodies touched and how hard; pass its `cursor` back to read only new events. `character_move` walks character controllers. Joints (`joint` component) are authored with `scene_update`. Pair these with `debug_pause` / `debug_step` for repeatable experiments.

Gameplay is written as Lua scripts: read the `quat://docs/scripting` resource, write files with `script_create` / `script_update` (compiled first; errors name the line), attach them with the `script` component, run with `sim_play` and read `debug_log` (category `script`).

Performance: `profile_frame` shows where the frame time goes (CPU and GPU per render pass and scene system), `profile_capture` records a number of frames and reports the costliest zones and the spikes over a frame budget, `profile_counters` the workload behind them (draws, triangles, GPU memory, entities). Measure before and after every change; the `find-performance-problem` workflow walks through it.

Custom looks: write a material shader (a Slang module) with `shader_template` → `shader_create`, assign it with `scene_update` (`primitive.shader`), screenshot and refine with `shader_update`; compile errors come back with file, line and column.

A game made here ships as a standalone package: `project_info` shows the open game project, `project_build` packages it for the engine's OS in the background and `project_build_status` reports progress and the archive.

If a tool says quat-mcp cannot connect to the engine or that the engine rejected the session, the engine is not running with remote control on that port, or its session token does not match: pass the error's advice on to the user instead of retrying. Never ask the user to paste the session token into the conversation.

Engine errors name the offending field and what was expected — fix the call instead of repeating it.

Step-by-step workflows for common tasks, with verification steps and pitfalls:
{% for skill in skills %}
- `{{ skill.name }}`{% if serve_skills %} (`{{ skill.uri }}`){% endif %}
{%- endfor %}
{% if serve_skills and serve_prompts %}
They are Agent Skills (read the `SKILL.md` with `resources/read`, or load them through your client's skills support) and prompts of the same name.
{%- elif serve_skills %}
They are Agent Skills: read the `SKILL.md` with `resources/read`, or load them through your client's skills support.
{%- elif serve_prompts %}
They are available as prompts of the same name.
{%- else %}
They are available as skills of the same name when the quat-3d plugin is installed.
{%- endif %} Follow the matching one when a task fits it.
