Inspect the scene in the running quat-3d engine using the quat-mcp tools and report what you find.
{%- if focus %}

Focus especially on: {{ focus }}
{%- endif %}

1. `engine_info` for the overall state (entity count, play mode).
2. `scene_list`, paging with offset until you have seen `total` entities; note the hierarchy (parent ids), names and which components each entity has.
3. `scene_get` on the entities that matter (lights, cameras, terrain, physics bodies, NPCs, and anything related to the focus) to read their component values.
4. `environment_get` and `camera_get`.
5. `camera_frame` with no entity, then `screenshot`; frame and screenshot specific areas if useful.

Then report: a short description of the scene, its hierarchy, lighting (sun + lights), physics and gameplay components, and problems you notice — unnamed or duplicate-named entities, objects floating above or sunk into the ground, overlapping geometry, dynamic bodies without colliders, nav agents without a baked navmesh, scenes too dark or blown out. Do not change anything unless asked.
