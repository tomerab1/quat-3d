# quat-3d Roadmap

The source of truth for what gets built next. It replaces the original
`PHASES.md` (still in git history), whose phases 1–14 are summarised under
[History](#history-phases-114).

## How work is organised

Work is grouped into **tracks** that can advance in parallel. Each item has an
ID (`S3`, `A2`, …) and ends in one or more commits titled `[<ID>] <description>`
(a multi-commit item uses `[<ID>.<n>]`). Rules:

- Finish (build warning-free, tests green, item ticked here) before starting
  an item's dependent work. Independent items in other tracks may interleave.
- Every item that adds behaviour adds a test the headless runner executes
  (see S3), or explains in its entry why it cannot.
- Tick the checkbox in the same commit that completes the item.
- If an item turns out to be wrongly scoped, edit this file first (in its own
  commit) rather than silently drifting.

Research basis for R11–R17 and G8–G10: [`docs/research/reports/Real time rendering tech radar.md`](docs/research/reports/Real%20time%20rendering%20tech%20radar.md).

**Current priority order:** S1 audit fixes (incl. the navmesh / NPC bugs) →
S3 GoogleTest → S7/S8 (thin `main.cpp`, split oversized files) → R9
instancing → rest of S → A (AI tooling / MCP) → P1/P2 (Mac) → G1 (audio) →
the rest by need.

**Owner's target game: [Cozy Builder](#games-built-on-the-engine-not-part-of-it)
(G16), in its own repository.** The owner decided this by default on
2026-11-28. Nothing above is reordered. **After the v0.1.0 release**, the
generic engine items G16 depends on take priority over the remaining
G/R/E work, in the order of
[`docs/design/cozy-builder.md` §9](docs/design/cozy-builder.md#9-phasing):
P8 → G17.1 → G5.1 → G15.1 → G18.1 → R23.1 → R23.2 → G19.1 → R9.5 →
G18.2/G18.3 → E1 → G6.1 → G11.1 → G17.2 → R25.1 → R25.2/R25.3 → R24 → R23.3.

---

## S — Stability & quality *(current focus)*

- [ ] **S1 — Bug audit.** Module-by-module review (RHI/renderer/frame loop;
  scene/physics/nav/net/animation; editor/assets/glTF/scene IO). Every confirmed
  defect fixed with a regression test where one is feasible.
- [ ] **S2 — Untrusted-input hardening.** The engine builds without
  exceptions, so `nlohmann::json::get/value` on a mistyped field aborts the
  process. All external JSON (scene files, behaviour trees, remote requests)
  goes through the checked reader (`engine/core/json_fields.hpp`); network
  packets and glTF accessors are bounds-checked.
- [ ] **S3 — GoogleTest suite.** `engine_tests` on GoogleTest (vcpkg `gtest`):
  CPU tests always, GPU tests through a shared surface-less Vulkan device
  fixture (lavapipe in CI), registered with CTest. Step 1 wraps the existing
  `run_*_self_test()` functions as test cases; step 2 moves their bodies out
  of engine sources into `tests/<module>/` (smaller engine files, real
  per-assertion failure messages).
- [ ] **S4 — CI.** GitHub Actions: Linux (clang + libc++, vcpkg binary cache),
  Windows (MSVC), macOS (MoltenVK). Builds, runs S3, uploads failure logs.
- [ ] **S5 — Sanitizers + validation gates.** ASan/UBSan preset; CI fails on
  any Vulkan validation error or sanitizer report.
- [ ] **S6 — Logging.** Replace ad-hoc `fprintf` with a logging layer
  (spdlog per CLAUDE.md): levels, categories, file sink, in-memory ring buffer
  the editor console and the MCP debugger read.
- [ ] **S7 — Decompose `main.cpp`.** The 3.6k-line frame loop becomes
  `Application` (window, device, lifecycle), `FrameRenderer` (graph build +
  submission), `EditorBridge` (editor requests) and `RuntimeServices`
  (terrain streaming, play mode, networking). Behaviour-preserving.
- [ ] **S8 — Split oversized files.** Enforce the file-size rule in
  CLAUDE.md: `gltf_loader.cpp` (validation / mesh / material / skin /
  animation / scene assembly), and any file past ~1000 lines. Next:
  `src/scene/component_json.cpp` (1076 lines after the #93 train and R22.1, past the limit already;
  master alone was 1061): move per-component codecs into their own files
  as `world_clock_json.cpp` / `component_json_animation.cpp` do.

## A — AI tooling: MCP server in Rust

Language models drive the engine through the Model Context Protocol: a Rust
MCP server (`tools/quat-mcp`) speaks MCP over stdio to the client and
JSON-RPC over loopback TCP to the engine.

- [x] **A1 — Remote-control transport.** `engine_remote`: loopback JSON-RPC 2.0,
  newline-delimited, token auth, poll-driven on the main thread, deferred
  replies. `QUAT_REMOTE=<port>`, `QUAT_REMOTE_TOKEN`. *(implemented, awaiting
  full-build verification)*
- [x] **A2 — World commands.** Entity create/update/destroy/reparent/list/get,
  every component through one shared JSON codec (also used by scene files),
  the data-driven `Primitive` component (shapes + PBR material, serialized),
  sun/sky/clouds, terrain, ground-height queries, batched commands with
  back-references.
- [x] **A3 — Frame services.** Viewport screenshots (PNG, non-stalling GPU
  readback), camera get/set/frame, glTF spawn, play/stop, save/load (paths
  confined to the project), navmesh bake.
- [x] **A4 — `quat-mcp` server.** rmcp-based, typed tool schemas, screenshots
  as MCP image content, reconnect handling, tests against a mock engine,
  setup docs for Claude Code / Claude Desktop.
- [x] **A4.1 — `quat-mcp` idiom pass.** Model-facing text moved to Markdown
  under `content/` (tool descriptions via `#[doc = include_str!]`, prompts as
  MiniJinja templates); `server.rs`/`client.rs` split by responsibility (one
  rmcp router per tool group); `clippy::pedantic` enforced.
- [ ] **S9 — Conflict-resistant registration.** Every feature PR edits the
  same central lists (top-level `CMakeLists.txt` source lists, the
  component table in `component_json.cpp`, `remote_bridge` method wiring,
  quat-mcp `server/mod.rs` tool lists, `inspector.cpp`), so each merge
  forces the next 3–5 PRs to re-resolve them. Move to per-module
  `CMakeLists.txt` (`add_subdirectory` per `src/<module>`), a component
  registry built from per-component registration files, remote method
  tables registered per module, quat-mcp tools grouped by module with one
  router merge line each, and per-component inspector files.
- [ ] **A5 — Debugging tools.** Log tail + filters (S6), Vulkan validation
  messages, frame/GPU timings, pause / single-step / time scale, entity watch,
  physics raycasts and contact queries, navmesh path queries.
- [ ] **A6 — Shader authoring.** Create/edit material shaders (Slang modules
  implementing the material interface), compile with diagnostics returned to
  the model, hot-swap (needs R2).
  - [ ] A6 follow-up: `shadow` and `forward` variants in
    `material_shaders::variants` — shadow casting that evaluates the material
    and honours its alpha cutout (today shadows use the mesh's silhouette),
    and alpha-blended material shaders in the transparent pass.
- [ ] **A7 — Behaviour authoring.** Build and edit behaviour graphs (G2) by
  tool call, with validation errors returned as data.
- [ ] **A8 — Live tuning.** Every renderer / post-process / material
  parameter readable and writable over the remote API and MCP from one
  settings model the editor also edits (schema with ranges and units),
  saved with the scene; a `tune_look` workflow prompt.
- [x] **A9 — World-building ergonomics** (from MCP world-building sessions,
  issues #37–#42): scene-derived camera clip planes (+ `camera.set`
  near/far/cut); shading-only terrain edits without regeneration;
  batch-consistent `world.ground_height` + `terrain.status`; entity world
  bounds + `world.place_on_ground`; screenshots from an explicit camera
  (a one-frame camera swap that leaves the viewport camera untouched — the
  renderer has a single view); `asset_search` / `asset_fetch` for CC0 Poly
  Haven models (licence recorded, cooked with R17's `quat-cook` when
  installed).
- [ ] **A10 — Gameplay control** (design:
  [`docs/research/mcp-gameplay-control.md`](docs/research/mcp-gameplay-control.md)).
  Runtime control of a playing game over remote + MCP, all mutations
  reverted by Stop:
  - **A10.1 Physics** *(L)*: engine event ring (`events.poll`: contacts,
    triggers, script errors, sim state, scene changes); body velocities,
    impulses / forces, one gravity setting (Jolt + characters, saved with the
    scene), overlap / shape-cast / all-hits queries, current contacts;
    rigidbody friction / restitution / damping / CCD / axis locks; a `joint`
    component on Jolt constraints (motors, limits, break force);
    `character.move`.
    Landed (`feat/a10-physics-control`, fixes #47 #48): the event ring with
    contact / trigger events (`events.poll`), `physics.get/set/apply`,
    gravity, `raycast_all` / `overlap` / `shape_cast`, friction /
    restitution / gravity factor / damping, the `joint` component (fixed,
    point, hinge, slider, distance; limits, motors), `character.move`, the
    quat-mcp tools. Still open: script-error / sim-state / scene-change
    events, `physics.contacts` (current contacts), CCD and axis locks, joint
    break force.
  - **A10.2 Behaviour runtime** *(M)*: a blackboard (component, `bb_*`
    leaves, Lua access); `bt.blackboard.*`, per-tick trace, breakpoints that
    pause the sim clock, forced leaf results.
  - **A10.3 Script runtime** *(M, needs G7/#22)*: list instances with
    errors and timings, inspect `self`, call exported functions, sandboxed
    `eval` (same limits, captured output), restart, binding reflection.
  - **A10.4 Play-mode control** *(M–L)*: checkpoints (ECS + Jolt
    `StateRecorder`) with a determinism test, `sim.state`, `sim.run_until`
    (event / watch / log conditions), `input.inject` into the G7 input
    sampler, `camera.view`.
  - **A10.5** *(S each)*: animation state-machine params, audio bus mix,
    particle bursts, editor selection.
- [ ] **A11 — MCP protocol upgrades** *(M–L)*: annotation audit (rule
  test, `anthropic/maxResultSizeChars`); structured output (`outputSchema`)
  for stable tools; resources, templates, resource links and completions;
  subscriptions over the A10.1 event ring (legacy + `subscriptions/listen`);
  engine jobs (`jobs.*`, progress, cancel) mapped to MCP tasks (SEP-2663)
  and progress notifications; elicitation confirms for destructive tools
  (MRTR + legacy, `confirm: true` fallback); icons; the test suite on
  protocol 2025-11-25 and 2026-07-28. Roots / sampling / logging stay
  unused (deprecated).
- [ ] **A12 — Skills** *(M)*: quat-3d workflows as Agent Skills
  (research: [`docs/research/mcp-skills.md`](docs/research/mcp-skills.md)).
  - [x] **A12.1 Skills, serving, plugin.** `tools/quat-mcp/content/skills/`
    (build-level, tune-look, debug-physics-glitch, write-gameplay-script,
    design-npc-behaviour, author-material-shader, playtest, plus
    inspect-scene and debug-session from the old prompts), served over MCP
    with the skills extension (SEP-2640, rmcp 3.12), rendered as the MCP
    prompts (replacing `content/prompts/`), packaged as a Claude Code plugin
    (`tools/quat-mcp/plugin/`) + marketplace (`.claude-plugin/`); sync tests
    keep skills, tool names, resources, prompts and the plugin copy
    consistent. Built on the tools on the base branch (A5, A6, A7, A8, A9,
    G7): author-material-shader covers factors and A6 Slang material
    shaders (it replaces the `write_material_shader` prompt). The plugin
    runs a source build or the quat-mcp bundled in a release (P4,
    `project_root` option = the release's data folder).
  - [ ] **A12.2 Skills follow their tools.** Extend the skills as the tools
    they need land: debug-physics-glitch with A10.1 physics control (#54: velocities,
    contacts, events), playtest with A10.4 (`input.inject`,
    `sim.run_until`) and R4 profiling (`profile_*`).

## R — Rendering & performance

- [ ] **R1 — Async asset loading.** Thread-pool loads, `is_ready()` handles,
  fallback assets while loading, upload queue budgeted per frame.
- [x] **R2 — Shader hot reload.** File watcher → `slangc` → pipeline recreation
  on the next frame; compile errors surfaced, old pipeline kept on failure.
- [ ] **R3 — Asset hot reload.** Meshes/textures re-imported on change.
- [x] **R4 — Profiling.** *(owner request: "no prod engine could be taken
  seriously without it; the MCP should be able to query it")* Instrumentation
  is central (graph executor, system calls, frame loop), not sprinkled.
  - **R4.1 Profiling facade + Tracy.** `tracy` from vcpkg (overlay port:
    on demand, loopback-only listener) behind `ENGINE_PROFILE` (default ON
    for Debug / RelWithDebInfo, OFF for Release unless asked). One public
    header of macros (`QUAT_ZONE*`, `QUAT_FRAME_MARK`, `QUAT_PLOT`,
    `QUAT_THREAD_NAME`) feeding Tracy and the built-in profiler; Tracy
    headers stay out of every other header. Named main / worker / loader
    threads; one zone per render-graph pass, per scene system, and for the
    physics step, nav, scripting, audio, remote poll, editor and asset
    upload; plots for draws, instances, triangles and VMA budget / usage.
  - **R4.2 GPU zones + built-in frame profiler.** Per-pass GPU timing from a
    timestamp query pool per frame in flight (`vkCmdWriteTimestamp2`, read
    back a frame later without stalling, availability-checked, honouring
    `timestampValidBits` / `timestampPeriod`, disabled gracefully without
    timestamps); TracyVkContext (calibrated where the device offers it)
    when Tracy is on. Allocation-free per-frame scope ring; rolling avg /
    p50 / p95 / max per pass and system (CPU and GPU) and per frame (CPU,
    GPU, present wait) over 240 frames; editor Profiler panel (frame graph,
    sortable CPU vs GPU table).
  - **R4.3 Remote commands.** `profile.frame`, `profile.capture` (async over
    frames; `profile.result {id}`: top zones by total and p95, spikes over a
    budget with their worst zones, per-frame totals), `profile.counters`,
    `profile.reset`; bounded inputs, compact replies.
  - **R4.4 MCP.** `profile_frame`, `profile_capture` (starts, then polls
    with bounded waiting), `profile_counters`; a "find a performance
    problem" workflow prompt.
- [ ] **R4.5 — Data-oriented CPU pass** *(after R4; benchmark-gated: each
  change is kept only when an R4 capture or a benchmark shows a win)*:
  a per-frame linear / arena allocator in core; hot / cold splitting of
  components (strings, vectors and `shared_ptr` out of hot components such
  as `MeshSource`, `BehaviorTree`, `NavAgent`, `FootIK`; names resolved to
  indices at load); a compact TRS local transform and a parent-first
  hierarchy sweep; an `entt::group` for render extraction; cache-line
  padding of per-thread data; a glm SIMD trial behind the layout
  `static_assert`s.
- [ ] **R5 — GPU-driven rendering.** Bindless materials, one indirect draw per
  pass, instance data in storage buffers.
- [ ] **R6 — GPU culling.** Frustum + Hi-Z occlusion culling in compute.
- [ ] **R7 — Shader graph runtime.** Node graph → generated Slang module
  implementing the material interface (no `#ifdef` variants).
- [ ] **R9 — Instanced rendering (trees, rocks, grass).**
  - ✅ **R9.1 Instanced draws.** (cycle 1) The renderer batches identical mesh+material
    draws into one instanced draw; shaders read per-instance transforms
    through a buffer device address (GBuffer + shadow passes), deriving the
    normal matrix per instance (fixes audit A13). Existing per-entity
    MeshRenderers are batched automatically.
  - ✅ **R9.2 `InstancedMesh` + `Scatter`.** (cycle 2) One entity carries thousands of
    placements; seeded procedural scattering on terrain (density, slope /
    height limits, scale/rotation jitter) for trees and rocks. Serialized,
    inspector + remote/MCP editable.
  - **R9.3 GPU grass.** Compute-generated, wind-animated grass blades around
    the camera (the standard approach for dense ground cover).
  - **R9.4 Instance culling + LOD.** GPU frustum/occlusion culling per
    instance (with R6) and distance fade / LOD selection.
  - **R9.5 Placement along curves, on surfaces, masks and brushes**
    (design: [`docs/design/cozy-builder.md` §4.5](docs/design/cozy-builder.md#45-placement-along-curves-on-surfaces-and-brushes-r95)).
    Scatter domains beyond rect / circle: along a G18 spline or curve
    network (1D spacing with jitter, bands across the curve, tangent / normal
    alignment), on mesh surfaces (area-weighted triangle sampling with
    normal filters, `DynamicMesh` included); inclusion / exclusion masks
    (distance to curves, polygons, volumes; G19 paint channels); a
    `ScatterBrush` of replayable add / erase strokes, each seeded, so undo
    is popping a stroke. Deterministic as R9.2. *(needs G18.1, G19.1 for
    paint masks)*
- [ ] **R10 — GPU compute migration.** Move data-parallel work to compute
  passes (CLAUDE.md "GPU compute first"):
  - ✅ **R10.1 Clustered light culling on the GPU.** (cycle 1) Phase 7.2 shipped a flat
    per-pixel loop over every point light (max 64) — no clustering. A
    compute pass bins lights into view-space clusters (froxels); deferred
    lighting and the forward transparent pass read only their cluster's
    list; thousands of lights; spot lights fit the same path.
  - **R10.2 GPU terrain synthesis.** FBM + domain warp in compute (seconds
    → milliseconds per tile), GPU erosion; the deterministic CPU generator
    stays as the reference for seams, physics and tests (read back async).
  - **R10.3 GPU particles.** Compute-simulated emitters (spawn, integrate,
    collide with depth, sort) drawn instanced.
  - **R10.4** GPU instance/occlusion culling — tracked as R6 + R9.4.
- [ ] **R8 — Image quality.** GTAO, screen-space reflections, contact shadows,
  optional ray-traced shadows/AO, CUBICSPLINE animation interpolation.

- [ ] **R11 — Velocity + upscalers.** Complete per-object velocity buffer
  (skinned previous positions, instances, wind offsets, particles), reactive
  mask and one jitter contract feeding TAA and a single upscaler interface;
  FSR 2/3.1 first (MIT, runs on MoltenVK), then DLSS / XeSS / FSR 4.1 plugins
  on Windows/Linux. *(tech radar: quick win)*
- [ ] **R12 — Shadows + AO foundation.** Hi-Z depth pyramid (shared with R6),
  4-cascade CSM with GPU depth bounds and texel snapping, XeGTAO port with
  bent normals → visibility-bitmask SSILVB, contact shadows, then stochastic
  SSR. Supersedes the AO/SSR/contact-shadow parts of R8. *(quick win)*
  - [x] R12.11 Materials in the depth / shadow passes: glTF `MASK` alpha test
    (GBuffer + CSM casters, separate pipelines so opaque keeps early-Z,
    coverage-preserving alpha mips), translucent (coloured) shadows for
    transmissive / blended casters (#35, #23, #62).
  - [x] R12.12 Contact shadows: per-surface depth reconstruction (nearest
    texel along its own plane, neighbours chosen by second difference), so
    the march never hits a phantom surface interpolated across a silhouette
    (#86).
  - [ ] R12.13 Depth-slope prepass for contact shadows: a compute pass writes
    each texel's selected `depth_slope` (x/y) once, so a march step costs
    2 plain loads (depth + slope) instead of 2 gathers plus up to 4
    divergent loads; GTAO could reuse it for depth-derived normals.
- [ ] **R13 — Weather + wind.** One `WeatherState` (ECS singleton, Lua + MCP
  `set_weather`) driving a global wind field (grass, foliage, cloth, particles,
  ocean, clouds), wetness/snow layers, rain with an occlusion map, cloud
  shadows; then froxel volumetric fog + local fog volumes (on R10.1's
  clusters) and a 2.5D volumetric cloud layer. *(quick win → medium)*
  - [x] **R13.1 Weather + wind foundation.** One `WeatherState` singleton
    component (wind direction / speed / gustiness / turbulence, cloud
    cover, precipitation intensity + type, surface wetness, snow
    accumulation, temperature) with an A8-style field table driving the
    component codec (scene files), remote `weather.get` / `weather.set`,
    the quat-mcp tools and the editor's Renderer panel. A global wind field
    computed on the GPU (advected gust fronts + turbulence around the
    camera) that grass samples, with a CPU reference
    (`weather::wind_at`) for tests, physics and audio; clouds advect with
    it and take their cover from the weather. Wet surfaces (Lagarde's
    porosity-driven darkening / roughness and puddles on flat ground),
    snow accumulation on upward-facing surfaces (normal + height, lowering
    the terrain's snowline) and cloud shadows on the sun (a cloud shadow
    map computed on the GPU), composed with R12's CSM, all in the deferred lighting path through Slang interfaces.
  - [ ] **R13.2 Precipitation.** Rain / snow particles with an occlusion
    (rain-shadow) map, wind on particles, Lua `set_weather`, splashes and
    ripples, lightning. *(after R10.3 GPU particles and G7 Lua)* Also:
    weather transitions (eased changes), and with them anchor the wind
    lattice to the advected air: today a heading change rotates the gust /
    swirl lattices about the world origin, so far from it the gust fronts
    sweep sideways (integrate a 2D displacement in `WindAdvection` and keep
    the along / across anisotropy local).
  - [ ] **R13.3 Volumetrics.** Froxel volumetric fog + local fog volumes on
    R10.1's clusters, and a 2.5D volumetric cloud layer.
- [ ] **R14 — Foliage.** Main + detail wind bending, two-sided foliage shading
  model ID in the GBuffer, hashed alpha (coverage-preserving mips: R12.11), GPU
  instance cull + LOD cross-fade (with R9.4), octahedral impostors, GPU
  density placement from R10.2 terrain maps with **biome recipes authored
  over MCP**. *(quick win → medium)*
- [ ] **R15 — Global illumination tiers.** One world-space radiance-cache
  interface with pluggable tracers: probe volume (tier 0), compute SDF tracer
  à la AMD Brixelizer GI (tier 1, runs on macOS), `VK_KHR_ray_query` tracer +
  denoiser on RT GPUs (tier 2), ReSTIR / path tracing long-term. Needs R12.
- [ ] **R16 — Water.** FFT ocean (JONSWAP) with single-layer water shading and
  Jolt buoyancy from a deterministic CPU height reference; later heightfield /
  shallow-water local sims and rivers from R10.2 erosion flow. *(medium)*
- [x] **R17 — Asset cooker formats.** KTX2 + Basis Universal textures,
  meshoptimizer codecs and quantisation. *(quick win)* Simplified mesh LODs
  were cut from R17 (no consumer yet) and moved to R17.1.
  - [ ] **R17.1 Mesh LODs.** Cooker writes simplified index buffers
    (`meshopt_simplifyWithAttributes` with the normal stream), the loader
    validates and exposes them on `MeshAsset`, the renderer selects by
    screen size; round-trip test.
- [ ] **R18 — VFX graph (Niagara-class).** On R10.3's GPU particle core: a
  modular emitter model (spawn / init / update / render stages), a node
  graph compiled to Slang compute modules (shares R7's codegen), events
  between emitters, ribbons/trails, mesh particles, beams, GPU data
  interfaces (sample skeleton, mesh surface, terrain, depth/SDF, wind
  field), parameter collections, sub-UV + motion vectors, an editor graph
  (imgui-node-editor) with live preview, and MCP authoring of emitters.
  *(large; after R10.3, R7)*
- [ ] **R19 — Virtualized geometry (Nanite-class).** Cluster DAG built
  offline with meshoptimizer clusterlod (extends R17's cooker), page
  streaming, GPU-driven two-pass occlusion culling on R12's Hi-Z, a
  visibility buffer + material resolve pass, hardware raster through
  indirect draws (mesh shaders where `VK_EXT_mesh_shader` exists) plus a
  compute software rasteriser for sub-pixel triangles so every platform
  (MoltenVK included) has a path; foliage and instanced meshes first, then
  shadows (virtual shadow maps as a follow-up). *(very large; after R5, R6,
  R9.4)*
- [ ] **R20 — Cinematic camera + post stack.** A physical camera on the
  `camera` component (aperture, shutter, ISO → EV100 exposure after Lagarde
  & de Rousiers 2014; focal length + sensor → FOV; focus distance), a
  gather-based bokeh depth of field in compute (Jimenez 2014: half
  resolution, split near / far fields, CoC tile max + dilation, near-field
  bleeding), McGuire 2012 tile-max / neighbour-max motion blur from R11's
  velocity with the camera's shutter, AgX (Blender 4) and Khronos PBR
  Neutral beside ACES, colour grading (white balance, contrast /
  saturation / lift-gamma-gain in ACEScct, `.cube` look LUTs as validated
  untrusted input) and film extras (cos⁴ + artistic vignette, temporally
  stable grain, chromatic aberration), all in the A8 settings table, over
  MCP and in the editor. Post runs after TAAU at the display extent:
  TAAU → DoF → motion blur → bloom → exposure → tonemap / grade / film.
  *(medium; after R11)*
- [ ] **R21 — Lighting realism: SSR, HDRI IBL, energy-conserving specular.**
  Takes over R12's stochastic-SSR step. *(medium; needs R11, R12)*
  - [x] **R21.1 Energy-conserving specular.** DFG LUT with the punctual
    lights' height-correlated Smith term; multiple-scattering GGX for the
    environment (Fdez-Agüera 2019) and punctual lights (Kulla-Conty 2017
    compensation); horizon occlusion of normal-mapped reflections against
    the geometric normal from depth; A8 toggles; white-furnace tests.
  - [x] **R21.2 HDRI environments.** Guarded Radiance `.hdr` decode (limits,
    budget, fuzz test); GPU equirect → cube with rotation / intensity,
    filtered-importance-sampled GGX prefilter (Křivánek & Colbert 2008),
    SH9 irradiance, split-sum LUT; the HDRI's sun extracted into a shadowed
    directional light and clamped out of the maps; A8 `environment` group,
    `environment.set hdri`, scene files, editor; CPU-reference tests.
  - [x] **R21.3 Screen-space reflections.** Stochastic SSR (Stachowiak
    2015): GGX visible-normal rays marched through R12's Hi-Z pyramid,
    ray-reuse resolve, temporal accumulation on R11 velocity, IBL fallback
    with edge and roughness fades; A8 `ssr` group; mirror-plane test.
  - [ ] **R21.4 OpenEXR HDRIs** (tinyexr or OpenEXR core via vcpkg, with
    the same limits and fuzzing as `.hdr`).
  - [ ] **R21.5 SSR reprojection at the reflected point.** Reproject the
    SSR history through the hit's virtual position (surface + ray direction ×
    hit distance, stored by the resolve; surface velocity on misses), as
    Stachowiak 2015 / FidelityFX SSSR do, instead of the reflector's surface
    velocity, which trails when the camera moves over a mirror. GPU test: a
    camera pan over the mirror floor, error bounded against a history-free
    render. (PR #80 review.)
- [ ] **R22 — Time of day, sun & moon, night sky** *(owner request: "starry
  nights … a time system where the sun rises and sets, with a moon")*.
  Canonical reference: Jensen, Durand, Stark, Premože, Dorsey, Shirley, "A
  Physically-Based Night Sky Model" (SIGGRAPH 2001).
  - [x] **R22.1 World clock + astronomy** (simulation only, no shader
    changes). `engine_astro`: pure double-precision CPU ephemerides — UTC ↔
    Julian date with ΔT, Greenwich / local mean sidereal time, precession,
    nutation (low precision), the Meeus / NOAA low-precision Sun (≈ 0.01°)
    with the equation of time, sunrise / sunset / solar noon, the Meeus
    ch. 47 Moon (≪ 1°) with phase angle, illuminated fraction, bright-limb
    angle, optical libration and moonrise / moonset, refraction, airmass and
    extra-atmospheric sun / moon illuminance (lux), and the celestial-sphere
    rotation (J2000 → world) for the star field. Tested against Meeus'
    worked examples and published rise / set times. A `WorldClock` (one per
    scene, like `WeatherState`) advances on the sim clock (pause / step /
    time scale apply), derives a renderer-facing `SkyState` POD each tick
    and, when enabled, drives the scene's `DirectionalLight` so real
    sunrises and sunsets work with the existing atmosphere; disabled, nothing
    changes. Scene files, remote `time.get` / `time.set` / `time.skip`,
    quat-mcp tools, an editor Time of Day section and CI sunrise / noon /
    sunset screenshots and the Lua `time.clock` / `set_clock` / `skip` (#87).
  - [ ] **R22.2 Night sky rendering** (after #80 lighting realism and #64
    weather land; state of the art, physical units throughout):
    - *Atmosphere:* Hillaire 2020 with the Moon as a second light; airglow and
      zodiacal light (Jensen et al. 2001); a light-pollution skyglow parameter
      (Garstang model).
    - *Stars:* a tiered catalogue — bright stars (Hipparcos or Yale BSC) as
      energy-conserving, TAA-stable sub-pixel PSF splats in physical units,
      B−V → colour, airmass extinction from the transmittance LUT, refraction
      near the horizon, scintillation; faint stars and the Milky Way as an HDR
      sky texture (ESA Gaia sky map, CC BY-SA 3.0 IGO, or NASA SVS Deep Star
      Maps, public domain — licences verified before import) rotated by
      `SkyState`'s sidereal matrix.
    - *Moon:* NASA CGI Moon Kit textures; a Hapke / Lommel-Seeliger BRDF with
      the opposition surge; correct phase and libration (from `SkyState`);
      earthshine; moonlight as the shadow caster with a twilight hand-off
      from the sun.
    - *Perception:* wide-range histogram auto-exposure (R22.1's sunrise /
      sunset captures come out washed out: the current exposure over-brightens
      a low sun); scotopic / mesopic
      response with the Purkinje shift; night acuity loss and noise
      (Thompson, Shirley & Ferwerda 2002; Kirk & O'Brien 2011).
    - *Integration:* time-sliced IBL rebakes; moonlit volumetric clouds and
      fog; optional aurora and meteors.
    - *Validation:* photometric reference checks (full-moon ground
      illuminance ≈ 0.1–0.3 lux, moonless clear sky ≈ 0.001 lux, Sirius at
      magnitude −1.46) and twilight / night golden screenshots.
- [ ] **R23 — Runtime procedural geometry** (design:
  [`docs/design/cozy-builder.md` §4.4](docs/design/cozy-builder.md#44-runtime-procedural-geometry-r23)).
  Generic: any game that builds meshes at runtime (builders, roads, rivers,
  destruction debris).
  - [ ] **R23.1 Geometry kernel** (`engine_geometry`, CPU, deterministic,
    no graphics dependency): Clipper2 polygon offset / booleans / repair,
    earcut triangulation with holes, a weighted straight skeleton (own
    implementation; CGAL's is GPL) per Aichholzer et al. 1995, Felkel &
    Obdržálek 1998 with Eder / Held / Palfrader 2020's robustness lessons and
    Kelly & Wonka 2011 weights; profile sweeps along G18 frames, lofts,
    tapered extrusion; a `MeshData` builder (welding, crease normals,
    MikkTSpace tangents, box / planar / along-curve UVs, meshopt); seeded
    procedural stone meshes. Fixture + fuzz property tests.
  - [ ] **R23.2 Dynamic meshes.** `DynamicMesh` component + a
    `DynamicMeshStore` service: CPU `MeshData` uploaded through a staging
    ring with a per-frame byte budget, swapped at a frame boundary (old mesh
    draws until the new one is resident), retired via
    `DeferredReleaseQueue`; drawn through the normal batching / shadow /
    velocity paths. Per-instance custom data (`vec4`: tint, appear time) on
    `InstancedMesh` and `DynamicMesh` readable by the material interface;
    stable instance ranges per source key (partial updates); generated
    static colliders (boxes / convex / mesh, built on the job pool,
    owner = entity); navmesh dirty regions. No Vulkan types in the public
    API (P2). *(needs R9.2, R17; A6 #19 for emissive custom data)*
  - [ ] **R23.3 GPU-generated meshes.** Generator-registered Slang compute
    modules (A6 interface rules) writing vertex / index / instance buffers
    and indirect args from pooled buffers in a render-graph compute pass,
    each with a CPU reference the tests compare against (R10.2 pattern).
    *(needs R23.2, A6)*
- [ ] **R24 — Decals.** Deferred box-projected decals writing GBuffer
  albedo / normal / roughness with angle fade and sort order (Kim,
  SIGGRAPH 2012), instanced and culled like instances; clustered decals for
  the forward pass later (on R10.1). Remote / MCP / Lua placement. *(needs
  R12's depth inputs)*
- [ ] **R25 — Seasons** (design:
  [`docs/design/cozy-builder.md` §4.14](docs/design/cozy-builder.md#414-seasons-r25)).
  Generic season machinery; games supply per-asset content.
  - [ ] **R25.1 Season state + climate.** A `SeasonState` singleton:
    `year_phase` from the R22.1 Sun's ecliptic longitude (by hemisphere), or
    an independent phase rate, or manual. Season weights, and an option to
    drive the `WorldClock` date from the season (day length follows it while
    `fixed_day` loops the time of day). A `Climate` component (temperature,
    precipitation and cloud curves over the year) drives `WeatherState`
    through a seeded daily sequence, so R13's accumulation brings and melts
    snow. `season.*` remote / MCP / Lua. *(needs R22.1 #87, R13.1 #64)*
  - [ ] **R25.2 Seasonal materials and variants.** A Slang `ISeasonal`
    interface (no `#ifdef`): `ColourRamp` (1D ramp at phase + per-instance
    spread), `LeafDrop` (hashed alpha over a phase interval, applied in
    depth / shadow / velocity passes too), `Blossom`, and `SeasonalGrass` for
    R9.3. A material JSON `season` block. `SeasonalVariants` mesh sets per
    phase interval, chosen per instance. A particle-rate hook over the phase
    (falling leaves). *(needs R25.1, A6 #19, R12.11, R9.3)*
  - [ ] **R25.3 Snow accumulation masks.** A top-down precipitation
    occlusion map shared with R13's rain occlusion (Ohlsson & Seipel 2004;
    Reynolds, Laycock & Day 2015), re-rendered on geometry change. Coverage
    from accumulation × exposure × slope × material `snow_affinity` with
    noise, blended into the GBuffer. A terrain `snow_clear` paint channel,
    and grass flattened under snow. *(needs R25.1, R13.1 #64, G19.1)*
  - [ ] **R25.4 Frozen water**: ice and frost on water below 0 °C. *(needs
    R16)*

## P — Platforms & shipping

- [ ] **P1 — macOS via Vulkan-on-Metal.** MoltenVK/KosmicKrisp, harden the
  descriptor-set fallback path, feature checks instead of assumptions, macOS CI.
- [ ] **P2 — RHI backend seam.** Isolate Vulkan types behind backend
  interfaces (device, buffers, images, pipelines, command recording, render
  graph execution) so a native Metal backend can be added without touching the
  renderer. Slang already emits Metal Shading Language.
- [ ] **P3 — Steamworks layer.** Behind an engine interface (builds without
  the SDK): init/overlay, achievements, stats, cloud saves, rich presence,
  Steam networking (GameNetworkingSockets already matches its API). The SDK
  must be added to the repo by hand (Valve's partner download).
- [ ] **P6 — Release builds.** Tag-driven GitHub Releases with Linux,
  Windows and macOS (Vulkan-on-Metal) archives the owner can download and
  run; runtime asset-root resolution; smoke-tested in CI.
- [ ] **P7 — Signed releases.** Follow-up to P6: Developer ID signing +
  notarization (stapled) of the macOS .app so it opens with a plain
  double-click, Authenticode signing of the Windows build (SmartScreen);
  credentials as CI secrets, signing skipped on forks. Bundle a MoltenVK /
  Vulkan loader built for the engine's macOS 14 minimum (Homebrew bottles
  raise it to the runner's OS).
- [x] **P4 — Game packaging.** Game projects (`game.json`: name, reverse-DNS
  id, version, start scene, window, icon, content roots — strictly validated,
  format-versioned) with the sample `games/coin-collector`; the runtime player
  `quat-player` (same engine libraries and application layer, no editor, no
  hot reload, remote control only in development packages) built beside the
  editor; `quat-pack`: files collected by following references (scene codec,
  glTF URIs, Lua requires/literals), cooked with `quat-cook` when present,
  `manifest.json`, per-OS layout and smoke test through the P6 scripts
  (Linux tar.gz, Windows zip, macOS `.app` with Info.plist + icon from
  `game.json`); release-workflow packages per OS; editor (E7) and MCP
  (`project.*`) triggers. See docs/game-packaging.md. *Scope moved:* Steam
  depot layout → P3; follow-ups: Windows `.exe` icon resource, exporting from
  a downloaded editor release, cross-OS packaging.
- [ ] **P5 — Native Metal backend.** On top of P2.
- [ ] **P8 — Engine SDK** (design:
  [`docs/design/cozy-builder.md` §3.3–3.5](docs/design/cozy-builder.md#33-how-the-game-consumes-the-engine-an-installed-sdk-package-p8)).
  The installable, versioned CMake package that games in their own
  repositories build against:
  - `find_package(quat3d 0.N CONFIG)` with exported `quat3d::` targets;
  - only the stable public headers installed (no Vulkan types in them);
  - a bundled dependency prefix for the SDK's triplet;
  - `quat3d-toolchain.cmake`, with mismatched compilers refused;
  - `Quat3dGame.cmake` (`quat_add_game_module` / `quat_add_game`);
  - tools (`quat-cook`, `quat-pack`, `slangc`, `quat-mcp`), engine shaders
    and the public Slang interfaces;
  - per-OS SDK archives from the release workflow.

  Semver policy: 0.x breaks only in MINOR releases, with CHANGELOG
  migration notes and one-minor `[[deprecated]]` shims; from 1.0, breaks
  only in MAJOR releases. No ABI promise. Data formats are versioned
  separately and read backwards.

  Checks:
  - `public_headers_self_contained`;
  - `examples/sdk-consumer` built only against the installed package, as
    the `sdk_consumer` CTest on every PR and on all OSes before release
    upload;
  - a source-checkout override (`QUAT3D_SOURCE_DIR`) for co-development.

  *(needs P6/S4 #24, P4 #55; first SDK with v0.1.0 or the release after)*

## G — Gameplay systems

- [x] **G1 — Audio (miniaudio).** `AudioSource` / `AudioListener` components,
  3D spatialisation and attenuation, mixer buses, streaming music, editor
  preview, scene serialization.
- [x] **G2 — Behaviour node graph.** Visual node editor built on
  `imgui-node-editor` (thedmd, vcpkg port) over the existing data-driven
  behaviour trees (JSON stays the asset format); blackboard; more leaves
  (perception, animation parameters).
- [ ] **G3 — Shader node editor.** Visual front-end for R7 (same
  `imgui-node-editor` foundation as G2).
- [ ] **G4 — Networking.** Multiple players, entity spawn/despawn replication,
  interest management, RPCs, lag compensation, dedicated-server build.
- [ ] **G5 — Input actions.** Action maps over keyboard/mouse/gamepad,
  rebinding, saved per user.
  - [ ] **G5.1 Action maps** (design:
    [`docs/design/cozy-builder.md` §4.1](docs/design/cozy-builder.md#41-input-actions-g5)):
    JSON action-map assets (button / 1D / 2D actions; keys, mouse buttons,
    mouse delta, wheel, pointer, screen-edge zones, gamepad, chords),
    triggers (pressed / released / held / tap / hold / double-click / drag
    with a pixel dead zone), modifiers (dead zone, scale, invert, smoothing,
    camera-relative), a context stack (UI consumes pointer input over
    widgets), C++ service + Lua (`input.action`, G7's API kept) + remote
    `input.inject`; G7's built-in axes / actions become the default map.
    *(needs G7 #22)*
  - [ ] **G5.2 Rebinding + per-user persistence** (user overrides beside
    the project defaults; device glyph names for UI). *(needs G6.1)*
- [ ] **G7 — Game scripting (Lua 5.4 + sol2).** `Script` component with
  lifecycle callbacks, a sandboxed per-scene Lua state with instruction and
  memory limits, an API over entities/components (via the codec), input,
  physics, audio, navigation; hot reload; remote `script.*` + MCP tools so
  assistants can write gameplay; a small playable example.
- [ ] **G6 — In-game UI.** A runtime UI system for games (not ImGui).
  - [ ] **G6.1 Core subset** (design:
    [`docs/design/cozy-builder.md` §4.8](docs/design/cozy-builder.md#48-runtime-ui-subset-g61)):
    retained widget tree from JSON layouts (panel, row / column flex layout,
    icon button, toggle group, slider, thumbnail list / grid, label,
    tooltip, modal), theme tokens, DPI scaling, SDF text (Inter) and Lucide
    icons, G5 input routing (pointer over UI never reaches the world),
    bindings to actions and to values exposed by C++ modules and Lua, a pass
    after tonemap composited in sRGB (#102), keyboard / gamepad focus.
    *(needs G5.1)*
  - [ ] **G6.2 The rest**: rich text, animation, localisation tables,
    screen transitions.

- [ ] **G8 — Animation upgrades.** Inertialization, two-bone foot IK,
  ACL compression; then Jolt ragdolls / powered hit reactions, Jolt cloth,
  motion matching. *(quick win → medium)*
- [ ] **G9 — Hair + fur.** Shell + fin fur and hair cards with a hair shading
  model now; spike Jolt 5.6 GPU strand-hair simulation (must share the device
  and VMA); compute-rasterised strands long-term (64-bit atomics, check
  macOS). *(medium → long)*
- [ ] **G10 — Destruction.** Offline pre-fracture cooker, Blast-style bond
  graph on Jolt compound shapes, breakable constraints, debris tiers and a
  `DEBRIS` layer, bond-delta replication over the net layer, destruction
  fields over MCP. *(medium)*

**RPG / action-adventure set** (owner: "RPG games will be possible?"). With
G5 + G6 these make a small third-person RPG shippable via P4; all game data
is JSON/Lua so it is authorable over MCP.

- [ ] **G11 — Save / load game state.** Versioned save files (player,
  world deltas, script state via an explicit Lua `save`/`load` contract,
  quest/inventory state), slots + autosave, migration on version bump.
  - [ ] **G11.1 Save core** (design:
    [`docs/design/cozy-builder.md` §4.9](docs/design/cozy-builder.md#49-save--load-g111)):
    slots written atomically (version, game id + version, play time,
    thumbnail) holding chunks — an engine `entities` chunk (entities tagged
    `Saved`, through the component codec), `world_clock`, `season`, `weather`,
    `terrain_edits`, plus chunks game modules register with a version and
    migrations (unknown chunks kept); a `Transient` tag that scene files and
    saves skip (derived content); autosave; loads validated through
    `core::JsonFields`. *(needs G17.1)*
  - [ ] **G11.2 Lua save contract** (the `save` / `load` script functions,
    quest / inventory state with G12 / G13).
- [ ] **G12 — Dialogue + quests.** Data-driven dialogue graphs (choices,
  conditions, Lua hooks, localisable string tables) and a quest system
  (objectives, triggers, journal), each with an editor node view (reusing
  imgui-node-editor) and MCP authoring tools.
- [ ] **G13 — Gameplay framework.** Stats/attributes with modifiers,
  abilities with cooldowns/costs, items + inventory + equipment, loot
  tables — all data (JSON schemas) driven from Lua, validated on load.
- [ ] **G14 — Combat.** Hitbox/hurtbox shapes on skeleton joints, animation
  events/notifies and root motion (extends G8), damage pipeline, lock-on
  targeting, NPC combat behaviour-tree leaves.
- [ ] **G15 — Camera rigs.** Orbit/follow camera with collision
  (sphere cast), lock-on framing, smoothing, cutscene camera tracks.
  - [ ] **G15.1 Orbit / strategy rig** (design:
    [`docs/design/cozy-builder.md` §4.2](docs/design/cozy-builder.md#42-camera-rig-g151-orbit--strategy-rig)):
    a generic `CameraRig` component (focus, yaw, pitch, distance; pitch
    easing with log-distance; free yaw or yaw steps), every degree of
    freedom on an exact critically damped spring (Holden 2021, Lowe 2004;
    log distance for uniform zoom), key / stick / edge-scroll pan, grab-the-
    world drag pan with inertia, zoom toward the cursor, ground-following
    focus height and camera ground clearance, soft bounds, framing via
    `frame_box()`; optional `CameraRigInput` mapping G5 actions; remote
    `camera.rig.get / set / frame`, quat-mcp tools, Lua. *(needs G5.1)*
  - [ ] **G15.2 Third-person follow rig**: collision by sphere cast
    (A10.1), lock-on framing (G14).
  - [ ] **G15.3 Cutscene camera tracks** on G18 splines.

**Generic building blocks for game projects** (first consumer: the Cozy
Builder, G16; design: [`docs/design/cozy-builder.md`](docs/design/cozy-builder.md)).
Everything here is engine code with generic names; game rules never enter
the engine.

- [ ] **G17 — Game modules (extension points).**
  - [ ] **G17.1 Native game modules.** A game registers its own components
    (field tables → codec, remote schema, Lua, inspector rows; no ImGui in
    games), stateless systems in named phases (input / pre_physics /
    post_physics / post_transform / pre_render) with ordering, remote
    methods with JSON Schemas, Lua modules, G5 action maps, G6 screens and
    G11 save chunks through an `engine::game::Registrar`, with services
    (physics queries, terrain edits, dynamic meshes, assets, WorldClock,
    season, weather, job pool, command stack) behind Vulkan-free headers.
    Static C++ libraries are linked into per-game player + editor executables
    by the SDK's `quat_add_game()`: explicit generated registration, no
    plugin DLLs (no stable C++ ABI, macOS signing). `game.json` format 2
    adds `modules`, `mcp` and an `engine` version range; format 1 stays
    valid.

    **Layering rule + CI**: nothing in this repository depends on a game or
    on the in-tree samples, enforced by a `layering_check` CTest and a
    `QUAT_BUILD_SAMPLES=OFF` CI entry. Games build only against the
    installed SDK (P8). *(needs P8, S9, P4 #55, G7 #22)*
  - [ ] **G17.2 Declared game MCP tools and skills.** Module methods carry
    MCP metadata (tool name, Markdown description from the project's `mcp/`
    folder, annotations); `rpc.methods` returns schemas + metadata and
    quat-mcp registers them as dynamic tools on connect (name and
    collision checks, `tools/list_changed`), with no Rust code per game;
    game skills from `<project>/mcp/skills/` served beside A12's. The game
    repository's `.mcp.json` runs the SDK's matching `quat-mcp`. *(needs
    G17.1, A12 #83)*
- [ ] **G18 — Splines and curve networks.**
  - [ ] **G18.1 `Spline` component + curve library**: centripetal
    Catmull-Rom (Yuksel et al. 2011), cubic Bézier and polylines with
    per-point width / height / tags; arc-length tables and even-spacing
    sampling; up-locked and rotation-minimising frames (Wang et al. 2008);
    closest point, XZ intersections, offsets, bounds; ground projection via
    `GroundHeightFn`; codec, remote `spline.*`, MCP, Lua.
  - [ ] **G18.2 Curve networks**: endpoint snapping, splits at crossings,
    node classification (end / bend / T / X / n-way), ids stable under
    local edits. *(needs G18.1)*
  - [ ] **G18.3 Stroke capture**: pointer drag → ground hits → resample,
    Ramer–Douglas–Peucker, Schneider (1990) cubic fit → spline; loop
    closing, straight segments; shared by games and the editor's spline
    gizmo. *(needs G18.1, G5.1)*
- [ ] **G19 — Terrain editing.**
  - [ ] **G19.1 Edit layers**: an ordered, serialised list of stamps
    (polygon / curve capsule / circle with falloff): flatten to a height or
    plane, raise / lower, smooth, paint material channels and a grass mask;
    applied after generation per tile, deterministic (CPU reference + GPU
    path on R10.2 tiles), region-limited recompute that refreshes the
    terrain pass, Jolt height fields, Scatter revisions, the grass mask and
    nav regions; `terrain.edit.*` remote / MCP / Lua and a `TerrainEdits`
    service. *(needs R10.2)*

## E — Editor (production readiness)

- [ ] **E1 — Undo/redo.** Command stack covering every editor and remote edit.
  The stack is an engine library (`core::CommandStack` in `engine_core`:
  apply / revert, drag merging, transactions, memory budget, clean marker)
  that games use at runtime too (G16), not editor-only code.
- [ ] **E2 — Project + asset database.** Project file, stable asset GUIDs
  instead of paths, import settings.
- [ ] **E3 — Prefabs.**
- [ ] **E4 — Workflow.** Multi-select, copy/paste, snapping, focus, search.
- [ ] **E5 — Preferences + layouts.** Persisted settings, key bindings.
- [ ] **E6 — Asset thumbnails + previews.**
- [x] **E7 — Build & export dialog** (drives P4). File → Build & Export…:
  host target, output folder, shipping/development, optional smoke test;
  runs `quat-pack` in the background with progress, its log and a "Show in
  folder" toast. Restyled on E8's widget kit, toasts and command registry
  when those land.
- [ ] **E8 — Editor UI/UX redesign** *(owner request, top editor priority)*.
  A coherent, modern editor on par with Unreal 5 / Godot 4 / Blender:
  - [x] **E8.1 Design system** (`src/editor/ui/`): theme tokens (dark + light,
    one accent, semantic colours), Inter + JetBrains Mono + an icon font,
    DPI-aware sizing and spacing scale, and a widget kit — property grid
    rows (label column, reset-to-default, tooltips), XYZ-coloured vector
    fields, component headers (icon, enable toggle, ⋮ menu), search fields,
    icon buttons/toolbars, toasts, empty states.
  - [x] **E8.2 Shell**: main menu, main toolbar (play/pause/step, gizmo mode +
    space, snapping, camera speed), status bar (FPS/frame time, sim state,
    selection, remote/MCP connection), viewport as a dock panel with an
    overlay toolbar (view modes, stats), default layout + reset, layout
    persistence, a command palette (Ctrl+P) and consistent shortcuts.
  - [x] **E8.3 Core panels** on the kit: Hierarchy (search, type icons,
    F2 rename, drag re-parent, context menu), Inspector (property grid,
    searchable Add Component), Asset browser (breadcrumbs, grid/list, search,
    type icons), Console (A5 log ring, filters). The hierarchy visibility
    toggle moved to E8.4: the engine has no per-entity visibility yet.
  - [ ] **E8.4 Feature panels and follow-ups**: migrate the panels still on
    plain ImGui (Physics, Animation preview, the behaviour editor's toolbar
    and nodes, the Grass and Audio inspector sections) and those of later
    PRs (upscaler, particles, scripts) to the kit; hierarchy visibility
    toggle and an entity enable toggle (need an engine-side visible /
    enabled concept honoured by every draw path, not editor state in the
    ECS); wire Undo / Redo to E1's command stack; ImGui 1.92 dynamic font
    textures in `ImGuiPass` (free font scaling, thumbnails for E6).
    Done in the #52 follow-up PR: a forced `QUAT_EDITOR_THEME` is not saved;
    re-parenting under a zero-scaled parent is refused; scene paths must be
    valid UTF-8; escaping file symlinks are not listed; snap steps are
    clamped; swatch text follows the swatch's luminance.
  - **Verification:** real editor screenshots (headless capture of the
    docked editor) with before/after images for every slice:
    `scripts/editor-screenshots.sh` and `docs/screenshots/editor/`; the CI
    screenshot job captures the editor at 1600x900 once it adopts that
    script's settings.

---

## Games (built on the engine, not part of it)

Games live in **their own repositories** and build against a tagged engine
SDK (P8) through `find_package(quat3d CONFIG)`. They use only its stable
public headers, Lua, data and the remote/MCP API. Each game repository has
its own ROADMAP, STATUS and CI. The items here track each game's phases and
their **engine** dependencies, so that engine priorities stay visible.
Engine items never depend on game items.

- [ ] **G16 — Cozy Builder** *(owner's target game)*: a Tiny Glade-style,
  exterior-only, no-fail-state town builder with a ~60° tilted strategy
  camera, freehand walls / paths / fences / ivy and sketched building
  footprints procedurally generated with detail (stones, crenellations,
  gates, windows, doors, straight-skeleton roofs), adapting to the terrain,
  with day / night, weather and seasons for mood. The setting is a medieval /
  fairy-tale village. Repository: `cozy-builder`, which the owner creates
  and connects to Claude's GitHub app. Design:
  [`docs/design/cozy-builder.md`](docs/design/cozy-builder.md) (Part II
  moves to the game repository in G16.1).
  - [x] **G16.0 Design.** The document above: engine / game split, game
    modules, engine capabilities, generation rules, MCP tools, assets,
    performance targets, tests, phasing.
  - [ ] **G16.1 Project skeleton in the `cozy-builder` repository.**
    Needs from the owner: the empty repository, connected to Claude's GitHub
    app. SDK releases download from public GitHub Releases with no token.
    Then:
    - `engine.lock` and CI against the SDK tag (build, tests, Linux
      screenshots, an SDK-bump PR flow);
    - Part II of the design moved there;
    - `game.json` format 2 and module registration;
    - the fixed meadow scene (terrain + grass + WorldClock + SeasonState);
    - the camera rig with the owner's numbers and the input map;
    - `cozy_builder_tests`;
    - the asset fetch script with SHA-256 + licence records and a seasonal
      inventory.

    It runs and packages. *(needs P8, G17.1, G5.1, G15.1, P4 #55)*
  - [ ] **G16.2 Walls.** Strokes → wall curve network, junction rules
    (pillars, corner towers), profile sweep with terrain adaptation, stones
    in courses, crenellations, box colliders, picking, erase, ghost +
    validation; `build_wall_path`, `build_remove`, `build_query`,
    `build_validate`. *(needs G18.1–G18.3, R23.1, R23.2, R9.5)*
  - [ ] **G16.3 Buildings and roofs.** Footprints, merge groups, floors,
    CGA-style façade rules (door near a path, windows by bay),
    straight-skeleton hip / gable / mansard roofs and cone tower roofs,
    foundation flattening + plinth, towers; `build_house`, `build_tower`,
    `build_styles`. *(needs G16.2, G19.1, R23.1)*
  - [ ] **G16.4 Paths, gates, fences, ivy.** Brush-painted paths (terrain
    paint + decals + stepping stones), gates where paths cross walls and
    fences, ivy growth; `build_path`, `build_fence`. *(needs G16.2, G19.1,
    R24, R9.5)*
  - [ ] **G16.5 Decoration and mood.** Free props with surface alignment,
    foliage brush, lamps on at dusk from `SkyState`, window glow, weather
    presets; `build_place`, `build_paint_foliage`. *(needs R9.5, R22.1 #87,
    R13.1 #64, A6 #19)*
  - [ ] **G16.6 Build UI, undo / redo, save / load.** Tool bar, style strip,
    mood panel, menu + slots, autosave; `build_undo` / `build_redo`.
    *(needs G6.1, E1, G11.1)*
  - [ ] **G16.7 MCP and the `build-town` skill.** G17.2 declarations for
    every `build.*` tool, the skill, a demo town built from a command list,
    golden screenshots (noon / dusk / night / rain) in the game
    repository's CI. *(needs G17.2, G16.2–G16.6, #20's screenshot harness in
    the SDK)*
  - [ ] **G16.8 Scale and polish.** The performance targets (design §7) on
    a stress town, GPU detail generation where budgets miss, grow-in
    animations, tilt-shift DoF, a packaged release. *(needs R9.4, R23.3,
    R20 #79, R4 #82)*
  - [ ] **G16.9 Ambient life** *(optional, late phase)*: villagers, sheep
    and birds. Villagers and sheep use `NavAgent` + behaviour trees, birds
    are boids flocks, and the navmesh is refreshed on edits. *(needs R23.2)*
  - [ ] **G16.10 Seasons.** A climate profile; per-asset seasonal variants
    (ramps, leaf drop, blossom, bare variants); snow affinity in styles;
    roof snow caps; path `snow_clear`; seasonal audio and decorations; the
    season dial; four-season golden screenshots. *(needs R25.1–R25.3,
    G16.5; runs beside G16.6–G16.8)*

  Later, by default and not yet scheduled: generated maps (after the fixed
  meadow), gamepad support (after desktop), and water and bridges (after
  R16, with R25.4 freezing), which come before G16.9 ambient life. The
  default season pace is a 40-minute year with a 12-minute day, with a
  "follow the real calendar" option.

---

## History (phases 1–14)

Completed under the original phase plan (see `PHASES.md` in git history for
full per-slice notes):

1. **Foundation** — CMake + vcpkg, SDL3 window, Vulkan device, swapchain, VMA.
2. **Render graph** — Slang cooking + reflection, pipeline cache, descriptor
   buffers, render graph with automatic sync2 barriers.
3. **Assets** — asset manager, glTF meshes/textures, PBR material interface,
   deferred GBuffer + lighting + tonemap.
4. **ECS** — entt scene, systems, camera, glTF scene graph, PBR direct lighting.
5. **Animation** — skeletons, clips, animator, GPU skinning (compute), blending,
   state machine.
6. **Physics** — Jolt world, colliders/rigid bodies, character controller,
   triggers.
7. **Lighting** — CSM shadows, clustered point lights, sky, forward
   transparency, physically based transmission, IBL, texture mips/transforms,
   clearcoat, specular AA.
8. **Post** — TAA, bloom, auto-exposure.
9. **Editor** — ImGui docking shell, viewport, hierarchy, inspector, asset
   browser, animation preview, physics debug, gizmos/picking, play mode,
   scene save/load.
10. **Async loading / hot reload** — *not done; moved to R1–R3.*
11. **Atmosphere** — Hillaire sky LUTs, volumetric clouds, dynamic IBL.
12. **Terrain** — seeded generation + erosion, chunked LOD, Jolt height
    fields, seamless streaming.
13. **NPCs** — Recast navmesh, nav agents, behaviour trees, click-to-walk and
    patrol routes.
14. **Networking** — GameNetworkingSockets transport, snapshot replication,
    client-side prediction.
