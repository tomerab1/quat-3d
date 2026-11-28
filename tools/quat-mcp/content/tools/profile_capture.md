Record the next `frames` frames (default 120) and summarise them — the way to measure a change, or to catch hitches with their cause. The call starts the capture, waits until it has finished (up to `max_wait_s`, default 30 s) and returns the summary:

- `frame_ms`: total / cpu / gpu / wait times over the captured frames (avg, p50, p95, max).
- `top_by_total` and `top_by_p95`: the `top` (default 10) zones that cost the most in total and at the 95th percentile, each with its CPU (`cpu`, inclusive of nested zones; `cpu_self`, its own time) and GPU `total` / `avg` / `p95` / `max` and how many `frames` it ran in. They are ranked by the larger of the CPU self and GPU value, so wrappers such as `update_world` do not crowd out the work inside them.
- `spikes`: how many frames went over `budget_ms` (default 16.667, i.e. 60 fps; use 33.3 for 30 fps, 8.33 for 120 fps) and the worst of them with their three most expensive zones by own time (`self_ms`, with the inclusive `ms`; `on`: `cpu` or `gpu`).
- `per_frame`: every frame's `total_ms` and `gpu_ms`, to see patterns (a spike every N frames, a slow climb).

If the wait runs out first, the reply says so and gives the capture `id`; call `profile_capture` again with just `{"id": <id>}` to keep waiting. Only one capture records at a time. A capture of a paused or minimised engine still advances (frames keep rendering), but measures nothing the game does while paused.

Example — measure a change: `profile_capture {"frames": 240}`, make the change, `profile_capture {"frames": 240}` again and compare `frame_ms` and the top zones.
