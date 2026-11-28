# Profiling

quat-3d has two profilers that share the same instrumentation (ROADMAP R4):

- **The built-in frame profiler** is always compiled in. It keeps rolling
  statistics for every render-graph pass, scene system and frame-loop scope
  (CPU and GPU), and feeds the editor's **Profiler** panel and the remote
  `profile.*` methods that the quat-mcp tools use.
- **[Tracy](https://github.com/wolfpld/tracy)** is compiled in when
  `ENGINE_PROFILE` is on. It gives you a full timeline of every thread, GPU
  zones, plots and per-frame zone statistics in the Tracy viewer.

## Building with Tracy

| `ENGINE_PROFILE` | Default |
|---|---|
| `ON` | `Debug` and `RelWithDebInfo` builds, and the `release` preset |
| `OFF` | plain `Release` builds (the `windows-release` and `macos-release` presets), CI's Linux no-editor game build and the packaged release builds |

```sh
cmake --preset release                      # Tracy on (the preset sets it)
cmake --preset release -DENGINE_PROFILE=OFF # Tracy off
```

The Tracy client comes from vcpkg (overlay port `ports/tracy`, version
0.13.1) as the manifest feature `profiling`. CMake asks for that feature
itself (`VCPKG_MANIFEST_FEATURES`, before `project()`) only when
`ENGINE_PROFILE` is on, so a build with `ENGINE_PROFILE=OFF` on any OS
neither downloads nor builds Tracy. Switching `ENGINE_PROFILE` on in an
existing build directory installs it on the next configure. It is built with `TRACY_ON_DEMAND`, so the game collects nothing until
a viewer connects: without a viewer, every zone costs one check. The overlay
also builds it with `TRACY_ONLY_LOCALHOST`, so the game listens for the viewer
on the loopback interface only, like the remote-control endpoint.

## Connecting the Tracy viewer

1. Get the **Tracy 0.13.1** viewer (the version must match the client): the
   Windows build from the [GitHub release](https://github.com/wolfpld/tracy/releases/tag/v0.13.1),
   or build `profiler/` from the same tag on Linux / macOS
   (`cmake -B build -S profiler && cmake --build build`).
2. Run the game (any build with `ENGINE_PROFILE=ON`).
3. In the viewer, connect to `127.0.0.1` (the game appears in the list of
   discovered clients). Tracy's default port is 8086; `TRACY_PORT=<port>` in
   the game's environment changes it.
4. To profile a game on another machine, forward the port over SSH:
   `ssh -L 8086:127.0.0.1:8086 <host>`, then connect the viewer to
   `127.0.0.1`.

What you see:

- **Threads**: `main` (frame loop, systems, render-graph recording),
  `physics worker N` (Jolt's job system), `asset decode N` (glTF texture
  decoding), `asset load` (particle flipbooks), `terrain generator`, `shader
  compiler`, `screenshot encoder`, `stderr capture`.
- **CPU zones**: one per render-graph pass (its recording time), one per
  scene system (`transform_system`, `animation_system`, `physics_system`,
  `character_system`, `bt_system`, `nav_agent_system`, `script_system`,
  `audio_system`, `net_update`, ...), `physics_step`, `remote_poll`,
  `editor_ui`, `upload_*`, and the waits (`wait_gpu`, `acquire_image`,
  `present`).
- **GPU zones** on the `graphics queue` context: one per render-graph pass,
  plus `skinning`. When the device offers `VK_KHR_calibrated_timestamps` (or
  the EXT), the GPU timeline is calibrated against the CPU clock.
- **Plots**: `draws`, `instances`, `triangles`, `vram_usage_mib`,
  `vram_budget_mib`.
- **Frames**: one frame mark per presented frame.

## The built-in profiler

### Editor panel

**View → Profiler** (tabbed along the bottom with Assets, Console, Animation,
Behaviour, Physics and Stats; closable, and its open state is saved with the
other editor preferences). It is drawn with the editor's design system
(`src/editor/panels/profiler.cpp`: theme tokens, widget kit, monospace
numbers):

- a toolbar: **Freeze** / resume, **Reset**, the zone kind (All, Passes,
  Systems, Scopes, Waits), the frame budget, and a filter field;
- the frame time, CPU time (frame minus waits), GPU time and wait time,
  each as average, p95 and max over the last 240 frames, and the fps;
- a graph of the frame (wall) time and the GPU time per frame against a
  dashed budget line (default 16.7 ms);
- a table of passes, systems and scopes with CPU self time (the zone's own
  work), inclusive CPU and GPU avg / p95 / max and calls per frame, kinds tinted by category and values over the budget in
  the error colour. Click a column header to sort. **Freeze** holds the
  current numbers while you read them; **Reset** clears the statistics.

### Remote control and MCP

With the remote endpoint on (`QUAT_REMOTE=<port>`), the profiler answers
`profile.frame`, `profile.capture` / `profile.result`, `profile.counters` and
`profile.reset` (contract in [`remote-control.md`](remote-control.md#profiling-roadmap-r4)).
quat-mcp exposes them as tools:

| Tool | What it does |
|---|---|
| `profile_frame` | The rolling profile: frame times and the costliest passes / systems, sortable by CPU or GPU, filterable by kind. |
| `profile_capture` | Records N frames (default 120), waits for the result (bounded, default 30 s) and returns the top zones by total and p95, the spikes over a budget with their worst zones, and per-frame times. Call again with `{"id": N}` to keep waiting. |
| `profile_counters` | Draws, instances, triangles, GPU memory per heap and entity counts. |

The `find-performance-problem` workflow (an Agent Skill in
`tools/quat-mcp/content/skills/`, also served as a prompt of the same name)
walks a model through a profiling session: baseline capture, CPU- or
GPU-bound, locate, test one fix at a time, report with numbers.

## How it works

### Instrumentation

All instrumentation goes through the macros in `engine/core/profile.hpp`,
which feed the built-in profiler and (with `ENGINE_PROFILE`) Tracy:

```cpp
#include "engine/core/profile.hpp" // .cpp files only: it includes Tracy

void my_system(entt::registry& r) {
    QUAT_ZONE();                                 // named after the function
    {
        QUAT_ZONE_C("my_step", engine::core::ProfileCategory::system);
        ...
    }
    QUAT_PLOT("my_counter", count);
}
QUAT_THREAD_NAME("my worker");                   // on a new thread
```

Instrumentation is central rather than sprinkled: the render-graph executor
times every pass (`RenderGraph::execute`), the system call sites in
`Scene::tick` and `WorldRuntime::simulate_slice` time every system, and the
frame loop times its own stages and waits. A new pass or system is profiled
without touching the profiler.

Inside a system, the subsystems that do enough work to matter have their own
scopes, so a slow frame is attributable below the system level:

| Area | Zones |
|---|---|
| Scripts (G7) | `script_system` (system) → `script_triggers`, `script_update` → `script_on_fixed_update`, `script_on_update`; `script_load_module` (first use of a file), `script_start` (Play), `script_reload` / `script_hot_reload` (file changes) |
| GPU particles (R10) | `particle_emitters` (system: spawn budget, pool ranges, flipbooks) → `particle_record` (emitter records, pass declaration), `particle_flipbook_upload`; the `particle_*` compute passes are timed on the CPU and GPU by the render graph |
| Upscaler (R11) | `upscaler` (backend choice, TAAU declaration) → `taa_history_resize` (only on a new render extent); `taa_resolve` is a timed render-graph pass |

Zone names are interned with the category of their first use, so a CPU scope
never reuses a render-graph pass's name.

### CPU side

Scopes on the main thread (the thread bound with
`FrameProfiler::bind_current_thread`) are recorded into a fixed-capacity
per-frame buffer — name id, start / end in nanoseconds, depth, thread; 24
bytes, no allocation, no lock. Other threads' scopes go to Tracy only.
`QUAT_FRAME_MARK()` closes the frame: each zone's calls are summed and pushed
into a 240-sample ring per zone, next to the frame's wall, CPU and wait time.
A zone's ring takes one sample per frame it ran in, so its statistics cover
its last 240 runs (per run), not the last 240 frames; a zone that has not
run for 240 frames drops out of the table. Percentiles are nearest-rank.

Every CPU zone has two times. **Inclusive** (`cpu`) runs from the scope's
start to its end, so it contains every zone nested inside it; **self**
(`cpu_self`) is that minus the inclusive time of its direct children — the
zone's own work. Records are pushed as scopes close, so children arrive
before their parent: `end_frame` keeps the closed children's time per
nesting depth and subtracts it when the parent arrives, in one pass with no
allocation. Rankings (the `cost` order, a capture's `top_by_*`, a spike's
worst zones) use self time, so `update_world` > `physics_system` >
`physics_step` reads as one cost in `physics_step` instead of three. GPU
zones are render-graph passes timed back to back; none nests in another, so
their self time is their time.

Zone names resolve once per call site: the `QUAT_ZONE` macros keep the id in
a static, and render-graph passes, whose graph is rebuilt every frame, look
their names up in a per-thread `core::ProfileNameCache`, so the name table's
lock is only taken the first time a name is seen.

### GPU side

`rhi::GpuProfiler` keeps one timestamp query pool per frame in flight. Each
pass is bracketed by `vkCmdWriteTimestamp2` at `ALL_COMMANDS`, so a pass's
time runs from the end of the work before it to the end of its own, and the
pass times add up to about the frame's GPU time. After the frame slot's
fence wait, the results are read without stalling
(`VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`, no
`WAIT`), masked to the queue's `timestampValidBits` and scaled by
`timestampPeriod`; they reach the frame profiler two frames after the frame
was recorded. A device whose graphics queue has no timestamps
(`timestampValidBits` 0) gets a disabled GPU profiler; the panel and
`profile.frame`'s `gpu_timing` say so.

**MoltenVK (macOS):** Apple GPUs sample timestamps at Metal encoder
boundaries, so passes recorded into the same encoder can read 0 ms while the
frame total stays correct; MoltenVK also reports a `timestampPeriod` other
than 1. Treat per-pass GPU times on macOS as approximate.

## Overhead

- Built-in profiler: two clock reads and one 24-byte store per scope; a few
  microseconds per frame to aggregate. Always on.
- Tracy without a viewer: one connection check per zone (on demand). With a
  viewer connected, Tracy's usual cost (tens of nanoseconds per zone) and its
  own GPU query pool.
- GPU timestamps: two per pass, read back without stalls.
