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
  animation / scene assembly), and any file past ~1000 lines.

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
- [ ] **A9 — World-building ergonomics** (from MCP world-building sessions,
  issues #37–#42): scene-derived camera far plane; shading-only terrain
  edits without regeneration; batch-consistent `world.ground_height`;
  entity world bounds + `world.place_on_ground`; screenshots from an
  explicit offscreen camera; `asset_search` / `asset_fetch` for CC0 Poly
  Haven models (licence recorded, cooked with R17).

## R — Rendering & performance

- [ ] **R1 — Async asset loading.** Thread-pool loads, `is_ready()` handles,
  fallback assets while loading, upload queue budgeted per frame.
- [x] **R2 — Shader hot reload.** File watcher → `slangc` → pipeline recreation
  on the next frame; compile errors surfaced, old pipeline kept on failure.
- [ ] **R3 — Asset hot reload.** Meshes/textures re-imported on change.
- [ ] **R4 — Profiling.** Tracy CPU zones on every system/pass, GPU zones via
  the Vulkan extension; in-editor frame-time breakdown.
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
- [ ] **R13 — Weather + wind.** One `WeatherState` (ECS singleton, Lua + MCP
  `set_weather`) driving a global wind field (grass, foliage, cloth, particles,
  ocean, clouds), wetness/snow layers, rain with an occlusion map, cloud
  shadows; then froxel volumetric fog + local fog volumes (on R10.1's
  clusters) and a 2.5D volumetric cloud layer. *(quick win → medium)*
- [ ] **R14 — Foliage.** Main + detail wind bending, two-sided foliage shading
  model ID in the GBuffer, hashed alpha with coverage-preserving mips, GPU
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
- [ ] **R17 — Asset cooker formats.** KTX2 + Basis Universal textures,
  meshoptimizer codecs and quantisation. *(quick win)*

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
- [ ] **P4 — Game packaging.** Engine-as-library + game project template,
  editor vs runtime builds, cooked asset bundles, shipping config, depot layout.
- [ ] **P5 — Native Metal backend.** On top of P2.

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
- [ ] **G7 — Game scripting (Lua 5.4 + sol2).** `Script` component with
  lifecycle callbacks, a sandboxed per-scene Lua state with instruction and
  memory limits, an API over entities/components (via the codec), input,
  physics, audio, navigation; hot reload; remote `script.*` + MCP tools so
  assistants can write gameplay; a small playable example.
- [ ] **G6 — In-game UI.** A runtime UI system for games (not ImGui).

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

## E — Editor (production readiness)

- [ ] **E1 — Undo/redo.** Command stack covering every editor and remote edit.
- [ ] **E2 — Project + asset database.** Project file, stable asset GUIDs
  instead of paths, import settings.
- [ ] **E3 — Prefabs.**
- [ ] **E4 — Workflow.** Multi-select, copy/paste, snapping, focus, search.
- [ ] **E5 — Preferences + layouts.** Persisted settings, key bindings.
- [ ] **E6 — Asset thumbnails + previews.**
- [ ] **E7 — Build & export dialog** (drives P4).
- [ ] **E8 — Editor UI/UX redesign** *(owner request, top editor priority)*.
  A coherent, modern editor on par with Unreal 5 / Godot 4 / Blender:
  - **E8.1 Design system** (`src/editor/ui/`): theme tokens (dark + light,
    one accent, semantic colours), Inter + JetBrains Mono + an icon font,
    DPI-aware sizing and spacing scale, and a widget kit — property grid
    rows (label column, reset-to-default, tooltips), XYZ-coloured vector
    fields, component headers (icon, enable toggle, ⋮ menu), search fields,
    icon buttons/toolbars, toasts, empty states.
  - **E8.2 Shell**: main menu, main toolbar (play/pause/step, gizmo mode +
    space, snapping, camera speed), status bar (FPS/frame time, sim state,
    selection, remote/MCP connection), viewport as a dock panel with an
    overlay toolbar (view modes, stats), default layout + reset, layout
    persistence, a command palette (Ctrl+P) and consistent shortcuts.
  - **E8.3 Core panels** on the kit: Hierarchy (search, type icons,
    visibility, F2 rename, drag re-parent, context menu), Inspector
    (property grid, searchable Add Component), Asset browser (breadcrumbs,
    grid/list, search, type icons), Console (A5 log ring, filters).
  - **E8.4 Feature panels** migrated once their PRs land (tuning, shadows/AO,
    upscaler, particles, scripts, behaviour editor, audio, grass).
  - **Verification:** real editor screenshots in CI (headless capture of the
    docked editor) with before/after images for every slice.

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
