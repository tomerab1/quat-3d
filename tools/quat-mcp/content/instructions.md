quat-mcp drives a running quat-3d game engine (Vulkan renderer, Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take screenshots to see the result.

Good workflow: `scene_components` and `scene_list` to inspect, plan with coordinates in metres (Y up), build with `scene_batch` (`"@N"` refers to the id created by command N of the batch), then `camera_frame` + `screenshot` and iterate with `scene_update`.

To change how the scene looks, tune the renderer (`render_settings_get` / `render_settings_set` / `render_settings_schema`: exposure, bloom, anti-aliasing, shadows, sky light, clouds) and per-entity materials (`material_get` / `material_set`), checking each change with `screenshot`; the `tune_look` prompt walks through it.

NPC behaviour is a behaviour tree per entity: `bt_leaves` lists the node kinds and leaves, `bt_set` writes a whole tree, `bt_get` shows it with stable node ids for the incremental `bt_add_node` / `bt_move_node` / `bt_remove_node` / `bt_set_params`, and `bt_status` shows what each node did on the last tick during Play.

Physics runs during Play (`sim_play`). `physics_get` / `physics_set` / `physics_apply` read and push bodies, `physics_set_gravity` changes the scene's gravity, and `physics_raycast_all` / `physics_overlap` / `physics_shape_cast` query the world. `events_poll` reports which bodies touched and how hard; pass its `cursor` back to read only new events. `character_move` walks character controllers. Joints (`joint` component) are authored with `scene_update`. Pair these with `debug_pause` / `debug_step` for repeatable experiments.

Engine errors name the offending field and what was expected — fix the call instead of repeating it. The `build_world`, `inspect_scene`, `debug_session`, `design_behavior` and `tune_look` prompts describe the workflows in detail.
