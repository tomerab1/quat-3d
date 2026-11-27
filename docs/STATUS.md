# Status & handoff

**Read this first when you pick the project up.** It records what is in
flight, what is verified, what is blocked, and what to do next. Whoever ends a
work session updates it (keep it short — history belongs in git and
`ROADMAP.md`).

_Last updated: 2026-11-27._

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

## Work in flight

| Item | State | Verified? |
|---|---|---|
| S1 bug audit | 46 findings recorded in [`audit/S1-findings.md`](audit/S1-findings.md) with severity and status. Fixed so far: B7, B11/C4, B13 (`[S1.1]`); C1–C3 + C5 index part — glTF structural validation (`[S1.2]`); B6/C8 orphaned Jolt bodies/characters swept (`[S1.3]`); B10/C7 behaviour-tree parsing hardened (`[S1.4]`) | Animation fix verified locally; physics/scene fixes await CI |
| S3 GoogleTest | Step 1 done: `tests/` (gtest via vcpkg), legacy self-tests wrapped (`tests/legacy/`), GPU fixture, CTest CPU/GPU split. Step 2 (move test bodies out of engine sources) not started | Harness built and run locally against a source-built gtest (RPC tests pass); full suite awaits CI |
| S4 CI | Linux job in `.github/workflows/ci.yml` | First runs in progress on GitHub — check the Actions tab |
| A1 remote transport | `src/remote/`, `include/engine/remote/rpc_server.hpp`, wired in `main.cpp` (`QUAT_REMOTE`) | Library compiled warning-free + self-test passed standalone; `main.cpp` wiring not yet compiled |
| A2 world commands | `Primitive` component declared in `components.hpp`; codec, resolver and commands not written | — |

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
- Keep `README.md` human-readable and current; keep this file current.

## Blockers

- **Local builds in cloud sessions:** vcpkg downloads sources from
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
   status. Suggested order — highest risk first: C5 joints part, A3–A5 + A7 (render-graph sync and pool eviction),
   then A1+A2+A14 **together** (TAA jitter/history — fixing A1 alone makes the
   image shimmer), B1–B5 (networking/prediction), the rest.
3. S2: move `scene_io.cpp` JSON reads onto `JsonFields` (`behavior.cpp` done in S1.4).
4. S3/S4 done → tick them in `ROADMAP.md`; add Windows/macOS CI jobs.
5. Then A2 → A3 → A4 (Rust server) → A5 (debugging tools).
