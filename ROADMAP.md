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

**Current priority order:** S1 audit fixes → S3 GoogleTest → S7/S8 (thin
`main.cpp`, split oversized files) → rest of S → A (AI tooling / MCP) → P1/P2
(Mac) → G1 (audio) → the rest by need.

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

- [ ] **A1 — Remote-control transport.** `engine_remote`: loopback JSON-RPC 2.0,
  newline-delimited, token auth, poll-driven on the main thread, deferred
  replies. `QUAT_REMOTE=<port>`, `QUAT_REMOTE_TOKEN`. *(implemented, awaiting
  full-build verification)*
- [ ] **A2 — World commands.** Entity create/update/destroy/reparent/list/get,
  every component through one shared JSON codec (also used by scene files),
  the data-driven `Primitive` component (shapes + PBR material, serialized),
  sun/sky/clouds, terrain, ground-height queries, batched commands with
  back-references.
- [ ] **A3 — Frame services.** Viewport screenshots (PNG, non-stalling GPU
  readback), camera get/set/frame, glTF spawn, play/stop, save/load (paths
  confined to the project), navmesh bake.
- [ ] **A4 — `quat-mcp` server.** rmcp-based, typed tool schemas, screenshots
  as MCP image content, reconnect handling, tests against a mock engine,
  setup docs for Claude Code / Claude Desktop.
- [ ] **A5 — Debugging tools.** Log tail + filters (S6), Vulkan validation
  messages, frame/GPU timings, pause / single-step / time scale, entity watch,
  physics raycasts and contact queries, navmesh path queries.
- [ ] **A6 — Shader authoring.** Create/edit material shaders (Slang modules
  implementing the material interface), compile with diagnostics returned to
  the model, hot-swap (needs R2).
- [ ] **A7 — Behaviour authoring.** Build and edit behaviour graphs (G2) by
  tool call, with validation errors returned as data.

## R — Rendering & performance

- [ ] **R1 — Async asset loading.** Thread-pool loads, `is_ready()` handles,
  fallback assets while loading, upload queue budgeted per frame.
- [ ] **R2 — Shader hot reload.** File watcher → `slangc` → pipeline recreation
  on the next frame; compile errors surfaced, old pipeline kept on failure.
- [ ] **R3 — Asset hot reload.** Meshes/textures re-imported on change.
- [ ] **R4 — Profiling.** Tracy CPU zones on every system/pass, GPU zones via
  the Vulkan extension; in-editor frame-time breakdown.
- [ ] **R5 — GPU-driven rendering.** Bindless materials, one indirect draw per
  pass, instance data in storage buffers.
- [ ] **R6 — GPU culling.** Frustum + Hi-Z occlusion culling in compute.
- [ ] **R7 — Shader graph runtime.** Node graph → generated Slang module
  implementing the material interface (no `#ifdef` variants).
- [ ] **R8 — Image quality.** GTAO, screen-space reflections, contact shadows,
  optional ray-traced shadows/AO, CUBICSPLINE animation interpolation.

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
- [ ] **P4 — Game packaging.** Engine-as-library + game project template,
  editor vs runtime builds, cooked asset bundles, shipping config, depot layout.
- [ ] **P5 — Native Metal backend.** On top of P2.

## G — Gameplay systems

- [ ] **G1 — Audio (miniaudio).** `AudioSource` / `AudioListener` components,
  3D spatialisation and attenuation, mixer buses, streaming music, editor
  preview, scene serialization.
- [ ] **G2 — Behaviour node graph.** Visual node editor built on
  `imgui-node-editor` (thedmd, vcpkg port) over the existing data-driven
  behaviour trees (JSON stays the asset format); blackboard; more leaves
  (perception, animation parameters).
- [ ] **G3 — Shader node editor.** Visual front-end for R7 (same
  `imgui-node-editor` foundation as G2).
- [ ] **G4 — Networking.** Multiple players, entity spawn/despawn replication,
  interest management, RPCs, lag compensation, dedicated-server build.
- [ ] **G5 — Input actions.** Action maps over keyboard/mouse/gamepad,
  rebinding, saved per user.
- [ ] **G6 — In-game UI.** A runtime UI system for games (not ImGui).

## E — Editor (production readiness)

- [ ] **E1 — Undo/redo.** Command stack covering every editor and remote edit.
- [ ] **E2 — Project + asset database.** Project file, stable asset GUIDs
  instead of paths, import settings.
- [ ] **E3 — Prefabs.**
- [ ] **E4 — Workflow.** Multi-select, copy/paste, snapping, focus, search.
- [ ] **E5 — Preferences + layouts.** Persisted settings, key bindings.
- [ ] **E6 — Asset thumbnails + previews.**
- [ ] **E7 — Build & export dialog** (drives P4).

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
