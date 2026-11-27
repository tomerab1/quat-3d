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
- [ ] **A12 — Skills** *(M)*: quat-3d workflows as Agent Skills in
  `tools/quat-mcp/content/skills/` (build-level, tune-look,
  debug-physics-glitch, write-gameplay-script, design-npc-behaviour,
  author-material-shader, playtest), served over MCP (SEP-2640 skills
  extension), packaged as a Claude Code plugin + marketplace entry, and
  rendered as the MCP prompts for clients without skills; tests keep
  skills, tool names and resources in sync. Each skill ships with the tools
  it needs (A10.x, A6, G7, A9).

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
- [ ] **R21 — Lighting realism: SSR, HDRI IBL, energy-conserving specular.**
  Takes over R12's stochastic-SSR step. *(medium; needs R11, R12)*
  - [ ] **R21.1 Energy-conserving specular.** DFG LUT with the punctual
    lights' height-correlated Smith term; multiple-scattering GGX for the
    environment (Fdez-Agüera 2019) and punctual lights (Kulla-Conty 2017
    compensation); horizon occlusion of normal-mapped reflections against
    the geometric normal from depth; A8 toggles; white-furnace tests.
  - [ ] **R21.2 HDRI environments.** Guarded Radiance `.hdr` decode (limits,
    budget, fuzz test); GPU equirect → cube with rotation / intensity,
    filtered-importance-sampled GGX prefilter (Křivánek & Colbert 2008),
    SH9 irradiance, split-sum LUT; the HDRI's sun extracted into a shadowed
    directional light and clamped out of the maps; A8 `environment` group,
    `environment.set hdri`, scene files, editor; CPU-reference tests.
  - [ ] **R21.3 Screen-space reflections.** Stochastic SSR (Stachowiak
    2015): GGX visible-normal rays marched through R12's Hi-Z pyramid,
    ray-reuse resolve, temporal accumulation on R11 velocity, IBL fallback
    with edge and roughness fades; A8 `ssr` group; mirror-plane test.
  - [ ] **R21.4 OpenEXR HDRIs** (tinyexr or OpenEXR core via vcpkg, with
    the same limits and fuzzing as `.hdr`).

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

**RPG / action-adventure set** (owner: "RPG games will be possible?"). With
G5 + G6 these make a small third-person RPG shippable via P4; all game data
is JSON/Lua so it is authorable over MCP.

- [ ] **G11 — Save / load game state.** Versioned save files (player,
  world deltas, script state via an explicit Lua `save`/`load` contract,
  quest/inventory state), slots + autosave, migration on version bump.
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
- [ ] **G15 — Third-person camera rig.** Orbit/follow camera with collision
  (sphere cast), lock-on framing, smoothing, cutscene camera tracks.

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
