# Status & handoff

**Read this first when you pick the project up.** It records what is in
flight, what is verified, what is blocked, and what to do next. Whoever ends a
work session updates it (keep it short — history belongs in git and
`ROADMAP.md`).

_Last updated: 2026-11-27._

## How work runs

As repeated improvement cycles — see [`CYCLE.md`](CYCLE.md) (up to 8 parallel
worktree agents per cycle, a scheduled task every 2 hours that resumes the
live session, one PR per cycle, merged once its CI is green) and [`LEARNINGS.md`](LEARNINGS.md).

## Current focus

Priority order agreed with the project owner: **stability (track S) → AI
tooling / MCP (track A) → macOS via Vulkan-on-Metal (P1/P2) → audio with
miniaudio (G1)**, then the rest of [`ROADMAP.md`](../ROADMAP.md).

Decisions already made (do not re-litigate without the owner):

- macOS: ship through Vulkan-on-Metal (MoltenVK / KosmicKrisp) now; keep the
  RHI ready for a native Metal backend later (P2 seam, P5 backend).
- Audio: miniaudio.
- MCP: a Rust server (`tools/quat-mcp`, rmcp crate) bridging to the engine's
  loopback JSON-RPC endpoint; the engine never speaks MCP itself.
- Steam: Steamworks behind an engine interface; the owner must add the SDK
  manually (Valve partner download).

## Claimed by the live session (do not pick up)

Cycle 1 is merged into `master` (PR #2, merge `c6acf19`, CI green: full Linux
build, CPU + lavapipe GPU tests, quat-mcp checks).

**Cycle 2** — eight worktree agents started from 473ec53; each pushes
`feat/<id>`. They merge into `claude/nifty-hopper-nb68qe` and land through the cycle-2 PR (#3).

| Item | Branch | Scope |
|---|---|---|
| S7.3 | `feat/s7-app-layer` | main.cpp → app layer (Application, FrameRenderer, EditorBridge, WorldRuntime, FlyCamera); fixes the ENGINE_EDITOR=OFF build. **Sole main.cpp restructurer** |
| G2 | `feat/g2-bt-editor` | Behaviour-tree node editor on imgui-node-editor; JSON ↔ graph model |
| A5 | `feat/a5-debug-tools` | `debug.*` remote methods (log ring, stats, pause/step/time scale, watch, raycast, nav path) + quat-mcp tools |
| R9.2 | `feat/r9-scatter` | `InstancedMesh` + seeded `Scatter` on terrain |
| R10.2 | `feat/r10-gpu-terrain` | Compute-shader heightmap synthesis, CPU reference kept |
| R2 | `feat/r2-shader-reload` | Shader hot reload (watcher, slangc service, deferred pipeline swap) |
| S1 batch 2 | `feat/s1-batch2` | B14, C5 joints, C11, C12, C14, C15, C16, A10, A15 |
| S4/P1 | `feat/s4-platform-ci` | Windows + macOS CI jobs and portability fixes |

## Work in flight

| Item | State | Verified? |
|---|---|---|
| S1 bug audit | 46 findings recorded in [`audit/S1-findings.md`](audit/S1-findings.md) with severity and status. Fixed so far: B7, B11/C4, B13 (`[S1.1]`); C1–C3 + C5 index part — glTF structural validation (`[S1.2]`); B6/C8 orphaned Jolt bodies/characters swept (`[S1.3]`); B10/C7 behaviour-tree parsing hardened (`[S1.4]`); C6, B12/C9, parts of C11/C16 via the component codec + scene_io rewrite (`[S2.1]`, `[S2.2]`); A5 transient-pool eviction (`[S1.5]`); A3/A4/A7 render-graph synchronisation (`[S1.6]`) | Animation fix verified locally; physics/scene fixes await CI |
| S3 GoogleTest | Step 1 done: `tests/` (gtest via vcpkg), legacy self-tests wrapped (`tests/legacy/`), GPU fixture, CTest CPU/GPU split. Step 2 (move test bodies out of engine sources) not started | Harness built and run locally against a source-built gtest (RPC tests pass); full suite awaits CI |
| S4 CI | Linux job in `.github/workflows/ci.yml` | First runs in progress on GitHub — check the Actions tab |
| S7 thin `main.cpp` | S7.1 + S7.2 done: tests, mesh generators and demo-scene construction (`src/app/demo_scenes.*`) moved out, `QUAT_SELFTEST` removed — 3,723 → ~2,010 lines. Next: the frame loop (`Application` / `FrameRenderer` / `EditorBridge`), and move the remote setup out of `main.cpp` | Compile-checked locally |
| S8 split oversized files | S8.1 done: `gltf_loader.cpp` (1,770 lines) → `src/scene/gltf/` (validate, mesh, material, skin_animation, scene_assembly, self_tests; largest 400 lines + 540 of legacy tests) with a private `internal.hpp` | Every file compile-checked locally; link verified by CI |
| A4 Rust MCP server | **Merged** (`tools/quat-mcp`): 27 tools, prompts, scene resource; 31 tests + fmt + clippy pass locally; Rust CI job added. Not yet tried against the real engine (A2/A3 in progress) | Verified locally |
| CI | Run 3: all vcpkg dependencies built + cached; engine build failed on a pre-existing `std::jthread` (libc++ 18) — fixed in `219b406`, next run in progress | — |
| A1 remote transport | `src/remote/`, `include/engine/remote/rpc_server.hpp`, wired in `main.cpp` (`QUAT_REMOTE`) | Library compiled warning-free + self-test passed standalone; `main.cpp` wiring not yet compiled |
| A2 world commands | Component codec done (`component_json.*`, 10 gtest cases pass locally); `Primitive` serialized but `resolve_primitives()` (mesh/material build) and the RPC commands not written | Codec verified locally |

Commit `1adbed7` is a WIP commit made before full-build verification; the
next session must confirm CI is green for it (or fix it) before building on
top.

### A2 design notes (agreed, not yet implemented)

- One JSON codec for components (`engine/scene/component_json.hpp`) used by both
  scene files and remote commands; scene file format bumps to version 2 (enums
  as strings, legacy integer enums still read). All reads go through
  `core::JsonFields` (strict for remote requests, lenient for files).
- Component names: `transform` (position / rotation in degrees / scale),
  `primitive`, `point_light`, `dir_light`, `camera`, `collider`, `rigidbody`,
  `character`, `nav_agent`, `behavior`, `patrol`, `terrain`,
  `net_replicated`, `animator` (edit only), `mesh` (read only).
- `Primitive` → `resolve_primitives()` builds the `MeshRenderer` from shared
  AssetManager keys (`primitive/mesh/<shape>`, `primitive/material/<hash>`);
  move the cube/sphere generators out of `main.cpp` into
  `src/scene/primitives.cpp`.
- Commands: `scene.list/get/create/update/remove_components/destroy/
  set_parent/clear/batch` (batch supports `"@N"` back-references to earlier
  results), `environment.get/set` (sun azimuth/elevation — same convention as
  the editor's Renderer panel — and clouds), `world.ground_height`.
- A3 adds frame-loop services via a `WorldServices` struct of callbacks:
  screenshot (non-stalling readback, PNG), camera, glTF spawn, play/stop,
  save/load (paths confined to the project root), navmesh bake.

## Standing instructions from the owner

- Keep `src/main.cpp` thin — new features get their own modules (S7).
- No very long files: aim under ~600 lines per `.cpp`, split past ~1000 (S8).
- Tests in GoogleTest under `tests/`; no new `run_*_self_test()` functions.
- Behaviour/shader node editors use `imgui-node-editor` (thedmd, vcpkg port).
- Use compute shaders wherever data-parallel work can move to the GPU (CLAUDE.md "GPU compute first", ROADMAP R10).
- Instancing for trees/grass is wanted (R9); the owner recalls navmesh/NPC bugs — a dedicated nav audit is part of S1.
- Keep `README.md` human-readable and current; keep this file current.
- Merge a cycle PR once its CI is fully green (CYCLE.md "Merge rule").
- `tools/quat-mcp`: idiomatic Rust built on well-known crates (rmcp, serde, schemars, thiserror/anyhow, tokio-util codecs, tracing, clap) — never hand-roll what a standard crate does.

## Blockers

- **Local builds in cloud sessions:** even on the `quat-3d` environment (which
  allows `codeload.github.com`), the proxy kept answering 403 for over 90
  minutes on 2026-11-27 — if it still does, ask the owner to check the
  environment's network settings. Details: vcpkg downloads sources from
  `codeload.github.com`. The default cloud environment blocks it; a personal
  environment named `quat-3d` allowing that host was created on 2026-11-27,
  but the proxy had not picked it up by the end of that session. In a new
  session select the `quat-3d` environment, then follow the cloud-container
  steps in [`DEVELOPMENT.md`](DEVELOPMENT.md#building-in-a-cloud-container).
  Until local builds work, CI is the source of truth for linking and tests.
  Meanwhile individual files can be **syntax/warning-checked locally**
  against header-only clones of the dependencies (plain `git clone` from
  GitHub works) — see "Compile-checking without vcpkg" in
  [`DEVELOPMENT.md`](DEVELOPMENT.md).

## Next steps

1. Check CI for the latest commit; fix compile errors in the WIP code.
2. Continue S1 from [`audit/S1-findings.md`](audit/S1-findings.md): re-verify
   each finding against the code, fix with a regression test, update its
   status. Suggested order — highest risk first: C5 joints part, then A1+A2+A14 **together** (TAA jitter/history — fixing A1 alone makes the
   image shimmer), B1–B5 (networking/prediction), the rest.
3. S2 remaining: bounds-check untrusted network packets (B2 input validation).
4. S3/S4 done → tick them in `ROADMAP.md`; add Windows/macOS CI jobs.
5. Then A2 → A3 → A4 (Rust server) → A5 (debugging tools).
