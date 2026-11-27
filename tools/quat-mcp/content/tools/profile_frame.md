Where the frame time goes right now: the engine's built-in profiler over the last 240 frames (about 4 s at 60 fps). Returns the frame times (`frame_ms`: `total` wall time, `cpu` = total minus `wait`, `gpu`, and `wait` = time blocked on the GPU or the display, each as avg / p50 / p95 / max in milliseconds), `fps`, whether GPU timing works (`gpu_timing`), the per-frame `counters` (draws, instances, triangles, VRAM) and a table of `zones`:

```json
{"name": "lighting", "kind": "pass", "cpu": {"avg": 0.04, "p95": 0.06, "max": 0.2}, "gpu": {"avg": 2.1, "p95": 2.4, "max": 3.0}, "calls": 1}
```

`kind` is `pass` (render-graph pass: `cpu` is the time to record its commands, `gpu` the time the GPU spent on it), `system` (scene systems: physics, animation, navigation, behaviour trees, scripts, audio, networking; summed over a frame's physics sub-steps), `scope` (frame loop, uploads, editor UI) or `wait`. Zones nest (a system inside `update_world`), so do not add rows up.

Read it like this: if `frame_ms.wait` is large, the CPU is waiting for the GPU — the frame is GPU-bound and the passes with the largest `gpu.avg` are the cost. Otherwise it is CPU-bound: look at `system` and `scope` rows. A `p95` or `max` far above `avg` means hitches; use `profile_capture` to catch them with their worst zones. Sort with `sort` (`gpu`, `cpu_p95`, ...), filter with `kind`, shorten with `limit`. Numbers are rolling: after changing something, wait a few seconds (or `profile_capture`) before comparing.
