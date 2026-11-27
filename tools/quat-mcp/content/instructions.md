quat-mcp drives a running quat-3d game engine (Vulkan renderer, Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take screenshots to see the result.

Good workflow: `scene_components` and `scene_list` to inspect, plan with coordinates in metres (Y up), build with `scene_batch` (`"@N"` refers to the id created by command N of the batch), then `camera_frame` + `screenshot` and iterate with `scene_update`.

Gameplay is written as Lua scripts: read the `quat://docs/scripting` resource, write files with `script_create` / `script_update` (compiled first; errors name the line), attach them with the `script` component, run with `sim_play` and read `debug_log` (category `script`). The `write_game_script` prompt walks through it.

Engine errors name the offending field and what was expected — fix the call instead of repeating it. The `build_world` and `inspect_scene` prompts describe the workflow in detail.
