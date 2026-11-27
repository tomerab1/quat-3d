# Status & handoff

**Read this first when you pick the project up.** It records what is in
flight, what is verified, what is blocked, and what to do next. Whoever ends a
work session updates it (keep it short — history belongs in git and
`ROADMAP.md`).

_Last updated: 2026-11-27 (factory running)._

## How work runs

As a software factory — see [`FACTORY.md`](FACTORY.md) (hourly Scanner and
Visual QA sessions open issues; one Reviewer session per PR; the live
orchestrator session fans issues and ROADMAP items out to worktree agents,
one `feat/<id>` branch + PR each) on top of the cycle mechanics in
[`CYCLE.md`](CYCLE.md) and the lessons in [`LEARNINGS.md`](LEARNINGS.md).

## Current focus

Priority order agreed with the project owner: **stability (track S) → AI
tooling / MCP (track A) → macOS via Vulkan-on-Metal (P1/P2) → audio with
miniaudio (G1)**, then the rest of [`ROADMAP.md`](../ROADMAP.md). The tech
radar ([`research/reports/`](research/reports/)) fed R11–R17 and G8–G10.

Decisions already made (do not re-litigate without the owner):

- macOS: ship through Vulkan-on-Metal (MoltenVK / KosmicKrisp) now; keep the
  RHI ready for a native Metal backend later (P2 seam, P5 backend).
- Audio: miniaudio. Scripting: Lua 5.4 + sol2 (G7).
- MCP: a Rust server (`tools/quat-mcp`, rmcp crate) bridging to the engine's
  loopback JSON-RPC endpoint; the engine never speaks MCP itself.
- Steam: Steamworks behind an engine interface; the owner must add the SDK
  manually (Valve partner download).

## Claimed by the live session (do not pick up)

Cycle 1 is merged into `master` (PR #2, `c6acf19`). **Cycle 2** is PR #3
(`claude/nifty-hopper-nb68qe` → `master`): A5, G2, R2, R9.2, R9.3, R10.2,
S1 batch 2, S3.3, S7.3, A7 and the PR #3 review fixes are merged into it;
review is non-blocking; it merges when CI on its head is green. Item PRs
below are stacked on it and get retargeted to `master` after it merges.

| PR | Item | Branch | State |
|---|---|---|---|
| #17 | A8 live tuning | `feat/a8-tuning` | review blocking (derived-material leak, A6 shader copy, shadow tests, base conflict) — worker fixing; merges before #19 |
| #18 | S1: issues #4 #5 #9 (RHI) | `feat/issues-4-5` | review clean; merges when CI is green |
| #19 | A6 shader authoring | `feat/a6-shader-authoring` | review blocking (source path confinement, timeout rollback, A8 composition, base conflict) — worker fixing |
| #20 | CI screenshots + audit D2/D7, fixes #7 #8 #21 | `feat/ci-screenshots` | #21 fixed (bodies spawned inside the floor); merging base to clear conflicts |
| #22 | G7 Lua scripting | `feat/g7-lua` | in review |
| #24 | S4/P1 Win + macOS CI, P6 release | `feat/s4-platform-ci` | review blocking (Windows UTF-8 paths, action SHA pins, base conflict) — worker fixing; tag `v0.1.0` after it merges |
| #25 | R12 shadows + AO foundation | `feat/r12-shadows-ao` | worker running |
| #26 | R10.3 GPU particles | `feat/r10-particles` | worker running |
| #28 | G8 animation quick wins | `feat/g8-animation` | worker running |
| — | R11 velocity + FSR | `feat/r11-velocity` | worker running (PR opens on first push) |
| — | R17 KTX2/Basis + meshopt | `feat/r17-cooker` | worker running |
| — | Mountain forest showcase | `feat/showcase-mountain` | worker running |

Triaged issues queued for the next free agents: #10 + #11, #14 + #23
(glass), #13 (aerial perspective → R13), #15 + #27 (terrain/grass look →
R14), #30 (Lua patterns).

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
