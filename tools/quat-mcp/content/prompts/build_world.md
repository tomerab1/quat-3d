Build this in the running quat-3d engine using the quat-mcp tools: {{ description }}
{%- if constraints %}

Constraints: {{ constraints }}
{%- endif %}

Work in this order:

1. **Inspect.** Call `scene_components` (which components exist and can be added), `scene_list` (what is already there), `environment_get` and `camera_get`. If the scene has terrain, check `terrain_status` (wait while it is generating) and query `world_ground_height` for the spots you will build on. For glTF models, `asset_list_models` lists what the project has; `asset_search` / `asset_fetch` add CC0 models from Poly Haven.

2. **Plan before creating anything.** Write a short plan: the layout with coordinates in metres (Y is up; flat ground is y = 0 unless there is terrain), and for each object its name, shape, position, rotation, scale and colour; the lights; the time of day and clouds. Give entities unique descriptive names and group parts under a parent (e.g. "House_1" with children "House_1_Walls", "House_1_Roof") so they can be moved together.

3. **Build in few round trips.** Use `scene_batch` with many scene.create commands; refer to an entity created earlier in the same batch with `"@N"` (its 0-based command index) in parent or entity fields. Primitives are about unit size and centred on their position: size them with transform.scale, and set position y to half the height so they rest on the ground. Add collider + rigidbody only where physics matters. Set spawned models and props on uneven ground with `world_place_on_ground` (it reads their bounds), not by guessing heights. Ground cover (lawns, meadows, fields) is one entity with a `grass` component, never individual blades; repeated objects (forests, rocks) are one `instanced_mesh` + `scatter`.

4. **Set the mood.** `environment_set` for the sun (elevation_deg 5..10 for golden hour, 45..70 for daylight) and clouds; point_light entities for lamps, fires and windows (warm linear colours such as [1.0, 0.6, 0.3]).

   **VFX.** Fire, sparks, smoke, steam, dust and falling leaves are one entity each with a `particle_emitter`, placed where the effect starts: `blend` additive with `emissive` > 1 for anything that glows, `blend` alpha with `lighting` lit and a slight upward `gravity` for smoke. Pair a fire with a warm point_light. Keep `max_particles` to what the effect needs (hundreds, not tens of thousands).

5. **Look.** `screenshot` with a `camera` (`{"frame": "<entity>"}`, or position + look_at): it leaves the user's viewport where it is. Use `camera_frame` / `camera_set` only when the user should see the view. Compare the image with the goal: proportions, gaps, objects floating or buried, overlaps, colours, lighting, composition. Take a second screenshot from another angle for anything large.

6. **Iterate.** Fix problems with `scene_update` (it merges: give only the fields that change) or another `scene_batch`, then screenshot again. Repeat until the result matches the description.

7. **Finish** with a short summary of what you built; save with `scene_save` if asked.

Conventions: rotation is Euler degrees [x pitch, y yaw, z roll] applied in YXZ order; scale is a number or [x, y, z]; primitive colours are "#rrggbb" sRGB hex; light colours are linear [r, g, b]. Engine errors name the bad field and what was expected — read them and correct the call instead of repeating it.
