---
name: playtest
description: Playtest a quat-3d level - enter play mode, let it run at normal and fast-forward speed, and check for script errors, physics objects leaving the world, stuck or failing NPCs, frame-time hitches, Vulkan validation errors and visual problems, then report each finding with evidence. Use before handing a level over, after a batch of changes, or when asked whether a quat-3d game works.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "engine_info scene_list debug_stats debug_log sim_play sim_stop debug_time_scale debug_pause debug_step debug_resume debug_watch script_list bt_status screenshot"
---

# Playtest a level

Run the game the way a player would leave it running, watch every system
that can fail on its own, and report findings with evidence. A playtest
changes nothing: `sim_stop` restores the scene as it was before Play.

## Workflow

1. **Baseline.** `engine_info` (must not already be playing; if it is,
   `sim_stop` first and say so). `debug_stats` for frame times and log
   counts, and `debug_log` with `{"level": "warn"}`; keep the returned
   `cursor` so later reads show only what the playtest produced.
2. **Pick what to watch.** `scene_list` for the actors: entities with
   `rigidbody` (`component` filter), `character`, `nav_agent`, `behavior`
   and `script`. Choose up to 64 of them.
3. **Start.** `sim_play`. After a second or two:
   - `script_list`: any script with `status.state` `error` or `failed` > 0
     is a finding; its `message` has `file:line`.
   - `debug_log` with `since_seq` set to your cursor: errors and warnings
     from scripts (`script`), trees (`bt`), navigation (`nav`) and the
     renderer (`vulkan`).
   - `screenshot` of the action.
4. **Let it run.** Watch the actors with `debug_watch` (restrict
   `components`, e.g. `["transform"]`) a few times, seconds apart, and
   compare positions:
   - a y position far below the ground or still falling: an object left the
     world (hand it to the debug-physics-glitch skill);
   - an NPC whose `nav_agent_state` shows `failed`, `path_partial` or a
     growing `stuck_s`: read its `bt_status` for the branch it is in;
   - entities that should move but do not.
5. **Fast-forward.** `debug_time_scale` `{"scale": 4}` for longer loops
   (spawners, day cycles, patrol rounds), then read the log, `script_list`
   and `debug_watch` again. Put it back with `{"scale": 1}`.
6. **Inspect moments.** For anything suspicious, `debug_pause`, then
   `debug_step` a few frames with `debug_watch` between steps, and
   `screenshot` the frozen frame. `debug_resume` afterwards.
7. **Performance.** `debug_stats` while playing: `fps`, `frame_time_ms`
   avg and worst (hitches show as worst far above avg), entity and draw
   counts compared with the baseline (a count that keeps climbing is a leak,
   e.g. a spawner that never destroys).
8. **Stop and report.** `sim_stop`, then `engine_info` to confirm play mode
   ended. Report each finding with its evidence (log line, values over time,
   screenshot), how to reproduce it, and the skill or fix that addresses it.
   Report "no problems found" only when every check above ran clean.

## Pitfalls

- You cannot press keys or move the mouse over MCP: anything that needs
  player input (movement, jumping, firing) is untested by this skill. Say
  so in the report and ask the user to try it.
- Physics bodies, scripts and trees exist only in play mode; `debug_stats`
  `physics` is `null` outside it.
- `debug_watch` positions are snapshots; compare two or more taken some
  time apart before calling something stuck.
- Leave the time scale at 1 and the simulation resumed when you finish.
