---
name: find-performance-problem
description: Find out why a running quat-3d game is slow or hitches - baseline frame capture, CPU- or GPU-bound, the costliest passes and systems, the spikes and their causes, the workload behind them - then test one fix at a time and report with numbers. Use when the user reports a low frame rate, stutter, hitches or a frame budget to reach.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "profile_capture profile_frame profile_counters render_settings_get render_settings_set render_settings_reset scene_list scene_get debug_log"
---

# Find a performance problem

Measure before you change anything, change one thing at a time, and measure
again. A guess about the bottleneck is not evidence; the profiler's numbers
are.

## Workflow

1. **Baseline.** `profile_capture` with `{"frames": 240}`, plus
   `"budget_ms"` when there is a target (1000 / target fps: 16.67 for 60 fps,
   8.33 for 120). Note `frame_ms` (avg and p95 of `total`, `cpu`, `gpu`,
   `wait`) and `spikes.count`. If GPU timing is missing, the device has no
   timestamps: judge by `cpu` and `wait`.
2. **Classify.** Large `wait` (the CPU blocked on the GPU) or `gpu` close to
   `total`: the frame is GPU-bound. Otherwise it is CPU-bound. Steady but
   slow means a constant cost; a low average with a high p95 / max or many
   spikes means hitches.
3. **Locate.** Zones nest, so a CPU zone has two times: `cpu` (inclusive:
   everything nested inside it counted too) and `cpu_self` (its own work).
   Rankings and spike zones use self time; a wrapper such as `update_world`
   with a large `cpu` but a small `cpu_self` only contains the cost.
   - GPU-bound: the passes at the top of `top_by_total` on the GPU side.
   - CPU-bound: the `system` and `scope` zones there; `profile_frame` with
     `{"kind": "system", "sort": "self"}` shows systems by their own time. Scripts, particles and the
     upscaler have their own zones (named `script_*`, `particle_*` and
     `upscaler*`).
   - Hitches: the `spikes` and their worst zones (`self_ms`), and
     `top_by_p95`. A zone's rolling statistics in `profile_frame` are per
     run, so a zone that runs every few seconds still shows its full cost.
     `per_frame` shows whether spikes repeat: every N frames points at
     periodic work such as streaming, uploads or script reloads.
4. **Explain.** `profile_counters` for the workload behind the cost (draws,
   instances, triangles, GPU memory against budget, entity counts),
   `render_settings_get` for expensive settings (shadow resolution and
   cascades, ambient occlusion, upscaler scale, bloom), `scene_list` /
   `scene_get` for the entities involved. `debug_log` with
   `{"level": "warn"}` can show related problems (hot reloads, failed
   uploads).
5. **Test a fix.** Change one thing (a render setting with
   `render_settings_set`, fewer or instanced entities, a cheaper material),
   let it settle, and run the same `profile_capture` again (a capture
   records new frames only). Keep the
   change only if `frame_ms` or the spikes improved; otherwise undo it
   (`render_settings_reset` restores a group's defaults).
6. **Report.** The bottleneck (CPU or GPU, which zone), the evidence
   (numbers before and after) and what fixed it or what would.

## Pitfalls

- The first frames after a scene load, a Play transition or a shader hot
  reload include one-off work (uploads, pipeline creation). Capture after
  the scene has settled, or the baseline is a hitch.
- Per-pass GPU times on macOS (MoltenVK) are approximate: passes in one
  Metal encoder can read 0 ms while the frame total is right.
- A capture runs over real frames: keep the camera and the scene still
  while comparing before and after, or the numbers measure different work.
