---
name: debug-session
description: Diagnose a problem in a running quat-3d game like a debugger - health stats, log tail, live entity watch, pausing and single-stepping the simulation, raycasts and navmesh path queries - and explain the cause with evidence before fixing anything. Use when something in a quat-3d game misbehaves (NPCs standing still, hitches, errors, rendering glitches) and the cause is unknown.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "engine_methods debug_stats debug_log scene_list scene_get debug_watch sim_play sim_stop debug_pause debug_step debug_resume debug_time_scale debug_raycast debug_nav_path screenshot"
---

# Debug session

Observe, form a hypothesis, test it. Do not change the scene until you know
the cause. For collisions and bodies that move wrongly, the
debug-physics-glitch skill goes deeper.

## Workflow

1. **Health check.** `debug_stats`: frame times (hitches show as a `worst`
   far above `avg`), entity and draw counts, physics bodies (`null` outside
   play mode), nav agents that `failed`, the simulation clock, and how many
   warnings and errors the log holds.
2. **Logs.** `debug_log` with `{"level": "warn"}`, and with
   `{"category": "vulkan"}` for rendering problems. Keep the returned
   `cursor`; pass it as `since_seq` later to read only what is new.
3. **Find the subjects.** `scene_list` (filter with `name_contains` or
   `component`) and `scene_get` for their full component values.
4. **Watch.** `debug_watch` the entities involved, restricting `components`
   to what matters (e.g. `["transform", "rigidbody"]`). For NPCs read
   `nav_agent_state`: `failed`, `path_partial`, `stuck_s`, `path`.
5. **Reproduce slowly.** `sim_play` if the problem needs the simulation,
   then `debug_pause` and advance with `debug_step` (1-10 frames at a
   time), calling `debug_watch` between steps to see exactly when a value
   goes wrong. `debug_time_scale` (e.g. `{"scale": 0.25}`) helps with fast
   events; `screenshot` shows a paused frame.
6. **Probe the world.** `debug_raycast` to see what is below or in front of
   something (the ground under a falling object, line of sight).
   `debug_nav_path` from an agent to its target shows whether a path exists
   or is `partial`.
7. **Conclude.** Explain the cause with the evidence (values, log lines, ray
   hits, paths) and propose the smallest fix; apply it only if asked, then
   verify with the same steps. Finish with `debug_resume`,
   `debug_time_scale` `{"scale": 1}` and, if you started it, `sim_stop`.

## Pitfalls

- A tool that reports `method_not_found` means the running engine is older
  or newer than this server: `engine_methods` lists what it implements.
- The log keeps the last 4096 lines; `missed` > 0 in the footer means lines
  were overwritten before you read them - read more often.
- Changes made during play are undone by `sim_stop`.
