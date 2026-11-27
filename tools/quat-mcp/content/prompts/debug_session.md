Diagnose a problem in the running quat-3d engine using the quat-mcp debugging tools.
{%- if problem %}

The problem: {{ problem }}
{%- endif %}
{%- if entity %}

Start with this entity: {{ entity }}
{%- endif %}

Work like a debugger: observe, form a hypothesis, test it. Do not change the scene until you know the cause.

1. Health check. `debug_stats`: frame times (hitches show as a high `worst`), entity and draw counts, physics bodies (`null` = not in play mode), nav agents that `failed`, the simulation clock, and how many warnings and errors the log holds.
2. Logs. `debug_log` with `level: "warn"`, and with `category: "vulkan"` for rendering problems. Keep the returned `cursor`; pass it as `since_seq` later to read only what is new.
3. Find the subjects. `scene_list` (filter with `name_contains` or `component`) and `scene_get` for their full component values.
4. Watch. `debug_watch` the entities involved, restricting `components` to what matters (e.g. `["transform", "rigidbody"]`). For NPCs read `nav_agent_state`: `failed`, `path_partial`, `stuck_s`, `path`.
5. Reproduce slowly. `sim_play` if the problem needs the simulation, then `debug_pause` and advance with `debug_step` (1–10 frames at a time), calling `debug_watch` between steps to see exactly when a value goes wrong. `debug_time_scale` (e.g. 0.25) helps with fast events; `screenshot` shows a paused frame.
6. Probe the world. `debug_raycast` to see what is below or in front of something (the ground under a falling object, line of sight). `debug_nav_path` from an agent to its target to see whether a path exists or is `partial`.
7. Conclude. Explain the cause with the evidence (values, log lines, ray hits, paths) and propose the smallest fix; apply it only if asked, then verify with the same steps. Finish with `debug_resume` and `debug_time_scale` `{"scale": 1}` so the game runs normally again.
