Find out why the running quat-3d engine is slow, using the quat-mcp profiling tools.
{%- if symptom %}

The symptom: {{ symptom }}
{%- endif %}
{%- if target_fps %}

The target: {{ target_fps }} fps (a frame budget of {{ budget_ms }} ms).
{%- endif %}

Measure before you change anything, change one thing at a time, and measure again.

1. Baseline. `profile_capture` with `{"frames": 240}`{% if target_fps %} and `"budget_ms": {{ budget_ms }}`{% endif %}. Note `frame_ms` (avg and p95 of `total`, `cpu`, `gpu`, `wait`) and `spikes.count`. If `gpu` timing is missing, the device has no timestamps: judge by `cpu` and `wait`.
2. Classify. Large `wait` (the CPU blocked on the GPU) or `gpu` close to `total`: the frame is GPU-bound. Otherwise it is CPU-bound. Steady but slow means a constant cost; a low average with high p95 / max or many spikes means hitches.
3. Locate. GPU-bound: the passes at the top of `top_by_total` on the GPU side. CPU-bound: the `system` and `scope` zones there (`profile_frame` with `{"kind": "system"}` shows systems only). Hitches: the `spikes` and their worst zones, and `top_by_p95`; `per_frame` shows whether spikes repeat (every N frames points at periodic work such as streaming or uploads).
4. Explain. `profile_counters` for the workload behind the cost (draws, instances, triangles, VRAM use against budget, entity counts), `render_settings_get` for expensive settings (shadow resolution and cascades, GTAO, upscaler scale, bloom), `scene_list` / `scene_get` for the entities involved. `debug_log` with `level: "warn"` can show related problems (hot reloads, failed uploads).
5. Test a fix. Change one thing (a render setting with `render_settings_set`, fewer or instanced entities, a cheaper material), wait a moment, and run the same `profile_capture` again. Keep the change only if `frame_ms` or `spikes` improved; otherwise undo it (`render_settings_reset` restores defaults).
6. Report. The bottleneck (CPU or GPU, which zone), the evidence (numbers before and after), and what fixed it or what would.
