quat-mcp drives a running quat-3d game engine (Vulkan renderer, Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take screenshots to see the result.

Good workflow: `scene_components` and `scene_list` to inspect, plan with coordinates in metres (Y up), build with `scene_batch` (`"@N"` refers to the id created by command N of the batch), then `camera_frame` + `screenshot` and iterate with `scene_update`.

To change how the scene looks, tune the renderer (`render_settings_get` / `render_settings_set` / `render_settings_schema`: exposure, bloom, anti-aliasing, shadows, sky light, clouds) and per-entity materials (`material_get` / `material_set`), checking each change with `screenshot`; the `tune_look` prompt walks through it.

NPC behaviour is a behaviour tree per entity: `bt_leaves` lists the node kinds and leaves, `bt_set` writes a whole tree, `bt_get` shows it with stable node ids for the incremental `bt_add_node` / `bt_move_node` / `bt_remove_node` / `bt_set_params`, and `bt_status` shows what each node did on the last tick during Play.

A game made here ships as a standalone package: `project_info` shows the open game project, `project_build` packages it for the engine's OS in the background and `project_build_status` reports progress and the archive.

Engine errors name the offending field and what was expected — fix the call instead of repeating it. The `build_world`, `inspect_scene`, `debug_session`, `design_behavior` and `tune_look` prompts describe the workflows in detail.
