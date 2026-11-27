quat-mcp drives a running quat-3d game engine (Vulkan renderer, Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take screenshots to see the result.

Good workflow: `scene_components` and `scene_list` to inspect, plan with coordinates in metres (Y up), build with `scene_batch` (`"@N"` refers to the id created by command N of the batch), then `camera_frame` + `screenshot` and iterate with `scene_update`.

Custom looks: write a material shader (a Slang module) with `shader_template` → `shader_create`, assign it with `scene_update` (`primitive.shader`), screenshot and refine with `shader_update`; compile errors come back with file, line and column.

Engine errors name the offending field and what was expected — fix the call instead of repeating it. The `build_world`, `inspect_scene`, `debug_session` and `write_material_shader` prompts describe the workflows in detail.
