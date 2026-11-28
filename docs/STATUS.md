# Status & handoff

**Read this first when you pick the project up.** It records what is in
flight, what is verified, what is blocked, and what to do next. Whoever ends a
work session updates it (keep it short — history belongs in git and
`ROADMAP.md`).

_Last updated: 2026-11-27 16:10 UTC (factory running; CI blocked by billing)._

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

Item PRs target `master` directly; `claude/nifty-hopper-nb68qe` carries only
orchestrator docs between merges.

Merged on 2026-11-27 after cycle 2:
- #18 (S1 RHI)
- #17 (A8 live tuning)
- #28 (G8 animation)
- #25 (R12 shadows + AO, fixes #16 #36)
- #57 (CI skips docs-only changes)
- docs PRs #33, #44, #50 (A10–A12 design), #51 (S9, G11–G15, R18, R19)

| PR | Item | Branch | State |
|---|---|---|---|
| #19 | A6 shader authoring | `feat/a6-shader-authoring` | review clear; R12 re-merge in progress (lighting.slang / lighting_pass.cpp) |
| #20 | CI screenshots + D2/D7, fixes #7 #8 #21 #45 #46 | `feat/ci-screenshots` | worker fixing 8 review items + master merge; blocker: a clean CI render of every scene |
| #22 | G7 Lua scripting | `feat/g7-scripting` | worker merging master (registration-list conflicts) |
| #24 | S4/P1 Win + macOS CI, P6 release | `feat/s4-platform-ci` | review clear; `b6e81b2` waits on CI (billing); tag `v0.1.0` after merge |
| #26 | R10.3 GPU particles | `feat/r10-particles` | review clear; R12 re-merge in progress |
| #34 | R11 velocity + TAAU | `feat/r11-velocity-upscale` | review clear; R12 re-merged `1f14ad6`; CI blocked (billing) |
| #43 | R17 KTX2/Basis + meshopt cooker (fixes #31) | `feat/r17-cooker` | review clear; CI on `cee5d48` |
| #49 | R14 mountain forest showcase | `feat/mountain-scene` | worker moving shadows onto R12's CSM + master merge |
| #52 | E8 editor UI/UX redesign | `feat/e8-editor-ux` | draft; worker running |
| #53 | A9 world-building (fixes #37–#42) | `feat/a9-worldbuilding` | draft; worker running |
| #54 | A10.1 physics control (fixes #47 #48) | `feat/a10-physics-control` | draft; worker running |
| #55 | P4 game packaging + E7 export | `feat/p4-game-packaging` | merged with train #93 (16e429c) and master (8103526); Export dialog ported to the E8 shell (`src/editor/shell/export_dialog.cpp`, Ctrl+B, toast actions); waits for CI, then lands after the train. Release PR #96 also edits `scripts/package-release.sh`: whichever lands second keeps one script with the engine release as the default mode and games as `--exe/--data` options (#96's quat-mcp copy is engine-release only; its licence files belong in game packages too) |
| #83 | A12.1 skills (MCP skills extension, prompts, Claude Code plugin) | `feat/a12-skills` | train #93 merged in (A6 shader + G7 script tools used by the skills; plugin works with the release-bundled quat-mcp); A12.2 waits on #54, A10.4, R4 |

Queued for the next free agents, in order:
1. R13 weather.
2. **#35 + #23**: alpha MASK and transmission in the depth/shadow passes.
3. Settings polish:
   - sharpness 0 when TAA is off;
   - lenient unknown choice names;
   - strict `render.settings.set` rejects retired keys;
   - transmission on a shader primitive.
4. G5 input, then G15 camera.
5. Then:
   - #10 + #11;
   - #14;
   - #13 (aerial perspective, goes with R13);
   - #15 + #27 (R14 look);
   - #30;
   - #29 (after #20).

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

- **GitHub Actions spending limit (since 2026-11-27 15:52 UTC).** The repo is
  private and hit its Actions spending limit. GitHub now refuses every new
  job ("recent account payments have failed or your spending limit needs to
  be increased"), so no PR can go green. Only the owner can fix this: raise
  the limit or fix the payment method under GitHub Settings → Billing &
  plans, or make the repo public. Usage cuts are in "Next steps" below and in
  LEARNINGS.md.
- **Local builds in cloud sessions:** vcpkg needs `codeload.github.com`, and
  the cloud proxy has refused it. Until local builds work, CI is the source of
  truth for linking and tests. Single files can still be compile-checked
  locally against header-only dependency clones; see "Compile-checking
  without vcpkg" in [`DEVELOPMENT.md`](DEVELOPMENT.md).

## Next steps

1. When Actions runs again, merge one PR at a time as each goes green:
   #43, #24 (then tag `v0.1.0`), #34, #26, #19. Re-merge `master` into a PR
   only when GitHub reports a conflict.
2. After #24 merges, cut CI usage:
   - cancel superseded PR runs;
   - with the owner's OK, run Windows/macOS on `master`, releases and
     manual runs only, not on every PR push.
3. Tick R10.3 in `ROADMAP.md` when #26 merges.
4. Start the queued items above as agents free up (8 at most).
