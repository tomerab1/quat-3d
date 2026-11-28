# Development guide

How to build, test and contribute. Architecture rules and coding conventions
are in [`CLAUDE.md`](../CLAUDE.md); the plan is in [`ROADMAP.md`](../ROADMAP.md);
the current state is in [`STATUS.md`](STATUS.md).

## Workflow

- Pick the next item from `ROADMAP.md` in the stated priority order (check
  `STATUS.md` for anything already in flight).
- Commits are titled `[<item ID>] <what changed>`, e.g. `[S1.3] fix cascade
  split overflow`; multi-commit items use `[<ID>.<n>]`.
- Before committing: the build is warning-free and `ctest` passes.
- New behaviour gets a GoogleTest case in `tests/<module>/<topic>_test.cpp`
  (picked up automatically by `tests/CMakeLists.txt`). Do not add new
  `run_*_self_test()` functions to engine sources.
- Tick the roadmap checkbox in the commit that completes an item, and update
  `docs/STATUS.md` at the end of each work session.

## Build presets

| Preset | Use |
|---|---|
| `debug` | clang + libc++, validation layers on |
| `release` | clang + libc++, optimised, Tracy compiled in (`ENGINE_PROFILE=ON`) |
| `windows-release` | MSVC 2022 (run from an x64 developer prompt), `x64-windows` triplet |
| `macos-release` | Apple clang, `arm64-osx-14` triplet (macOS 14 minimum), MoltenVK — sets `ENGINE_DESCRIPTOR_SETS_FALLBACK=ON` |

CMake options: `ENGINE_EDITOR` (default ON), `ENGINE_PROFILE` (the Tracy
client, R4: default ON for Debug / RelWithDebInfo, OFF for Release; it also
selects the vcpkg manifest feature `profiling`, so OFF builds never build
Tracy; see [`profiling.md`](profiling.md)),
`ENGINE_DESCRIPTOR_SETS_FALLBACK` (macOS / drivers without descriptor buffers),
`ENGINE_PACKAGE_BUILD` (release archives: no source-tree paths compiled in —
the game finds `assets/` + `cooked/shaders/` next to its executable or in
`QUAT_ASSET_ROOT`; see `src/app/asset_root.hpp`).

Targets: the engine libraries (`engine_core`, `engine_rhi`, `engine_renderer`,
`engine_scene`, `engine_physics`, `engine_animation`, `engine_terrain`,
`engine_nav`, `engine_net`, `engine_remote`, `engine_audio`, `engine_project`,
`engine_editor`), the `game` executable (the editor when `ENGINE_EDITOR` is
on), `quat-player` (the runtime player games ship with — always without the
editor) and `quat-player-shipping` (the same with remote control compiled out,
`QUAT_SHIPPING`; what shipping packages contain), the game packager
`quat-pack` (`engine_pack`), the offline cooker (`engine_cook` + the
`quat-cook` CLI, never linked into the game or the players) and the
GoogleTest suite `engine_tests` (`tests/`). `ENGINE_EDITOR` is a per-target
definition (the `engine_editor_build` interface target), not a global one, so
one editor build produces both executables from the same application sources
(`game_world` / `game_world_runtime` are the two builds of the window-free
world layer).

## Testing

```sh
ctest --test-dir build/release --output-on-failure
./build/release/engine_tests --gtest_list_tests       # every test
./build/release/engine_tests --gtest_filter='-Gpu*'    # device-free tests only
./build/release/engine_tests --gtest_filter='Nav.*:Ai.*'
./build/release/engine_tests --validation              # GPU tests under the validation layer
./build/release/engine_tests --require-gpu             # fail (not skip) GPU tests without a device
```

**Validation (`--validation` / `QUAT_VALIDATION=1`).** The shared GPU
device runs under `VK_LAYER_KHRONOS_validation`, and every error-severity
message fails the test that was running when it arrived, as
`Vulkan validation error in <Suite>.<Test>: <message>` (the fixture's sink,
`quat::test::report_validation_message`, passed as
`Device::CreateInfo::on_debug_message`). Errors outside any test (objects
leaked at device destruction, after the last test) fail the run. Warnings
are only logged. Asking for validation without the layer installed is an
error: there is no device, so GPU tests skip, or fail under
`--require-gpu` — a run never silently loses validation. Install the layer
with `apt-get install vulkan-validationlayers` (Linux) or the Vulkan SDK
(Windows, with `VK_LAYER_PATH` at the SDK's `Bin`). A test that creates its
own validated `Device` passes `quat::test::report_validation_message` too.

Without a GPU, point the Vulkan loader at Mesa's software driver:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/release/engine_tests
```

Test layout:

| Path | Contents |
|---|---|
| `tests/<module>/*_test.cpp` | Test cases (new tests go here) |
| `tests/support/expect_ok.hpp` | `ASSERT_OK` / `EXPECT_OK` / `ASSERT_ERR` for `std::expected` results |
| `tests/support/gpu_context.hpp` | Shared surface-less Vulkan device; start GPU tests with `QUAT_REQUIRE_GPU(ctx)` and name the suite `Gpu*` |
| `tests/support/render_readback.hpp` | GPU pass tests: `upload_mesh`, `render_to_host` (appends a read-back pass to a graph, runs it, returns the pixels), `read_rgba16f` (baked LUTs / probes) |
| `tests/legacy/` | Wrappers around the old `run_*_self_test()` functions, removed as their bodies migrate into `tests/<module>/` |


## Script hot reload

Editor builds watch `assets/scripts/` (and its subdirectories present at
startup) while running (`QUAT_SCRIPT_HOT_RELOAD=0` turns it off, `=1` turns
it on in non-editor builds). Saving a `.lua` file recompiles it; while
playing, its instances restart with the new code, and a compile or load
error keeps the previous version running (the error shows under **Stats →
Scripts**, in the log, and in `script.list`). Outside Play the file is only
syntax-checked. See [`scripting.md`](scripting.md).

## Shader hot reload

Editor builds watch `assets/shaders/` and `assets/shaders/lib/` while running
(`QUAT_SHADER_HOT_RELOAD=0` turns it off, `=1` turns it on in non-editor
builds). Saving a `.slang` file recompiles it with the build's slangc (same
flags as `cmake/CompileShaders.cmake`, into `cooked/shaders/`) on a worker
thread; editing a library module also recompiles every shader importing it.
The next frame swaps the rebuilt pipelines in; replaced pipelines are
destroyed once the frames in flight are done.

- A compile error keeps the previous pipeline and cooked files; the
  diagnostics show under **Stats → Shaders** and on stderr
  (`ShaderHotReload::statuses()` for tools). A slangc run that takes longer
  than `Config::compile_timeout` (60 s) is killed and reported the same way;
  shutting down kills a compile in flight instead of waiting for it.
- A shader used only by one-shot work (a bake pass whose pipeline is gone)
  compiles but has no live pipeline to swap: it is listed as **restart**
  ("compiled; no live pipeline uses it - restart to apply").
- Edits that change the pipeline layout (a new binding, a changed descriptor
  type, a bigger push-constant block, a renamed entry point) are rejected the
  same way — the C++ side builds the layouts, so they need a restart.
- Passes opt in by creating pipelines through
  `PipelineCache::create_compute_pipeline` / `create_graphics_pipeline` and
  holding the returned `rhi::ManagedPipeline`, reading `handle()` /
  `layout()` when recording.
- `QUAT_SLANGC=<path>` overrides which slangc is used.

## Material shaders

Authored materials (ROADMAP A6) are Slang modules in
`assets/shaders/materials/<name>.slang` implementing `IMaterial`
(`assets/shaders/lib/material.slang`) and ending with
`export struct SurfaceMaterial : IMaterial = <their struct>;` — start from
`assets/shaders/templates/material.slang`. Select one on an entity with
`primitive.shader` (+ `shader_params`), in the inspector or over remote
control (`shader.*` methods, see [`remote-control.md`](remote-control.md#material-shaders-roadmap-a6)).

- Each material is linked against the pass driver module
  `lib/material_pass_gbuffer.slang` (link-time specialisation: the driver
  declares `extern struct SurfaceMaterial`) into
  `cooked/shaders/materials/<name>.gbuffer.spv`, offline by
  `cmake/CompileShaders.cmake` and at runtime by shader hot reload — one
  program and pipeline per material, same layout as the built-in GBuffer
  program. Saving the file hot-swaps it like any other shader.
- Materials cannot declare resources: everything comes in through
  `MaterialInput` (uv, world position, normal, view direction, time, the
  entity's material constants incl. two `shader_params` vec4s, and the
  built-in PBR result `standard`). A program whose reflection does not fit the
  pass layout is rejected.
- They run in the deferred pass only (emission is a GBuffer target the
  lighting pass adds; alpha below 0.5 cuts out). Shadows do not run the
  material: a cut-out hole still casts a shadow (the mesh's silhouette). The
  variant table in `include/engine/rhi/material_shader_names.hpp` is where
  `shadow` and `forward` variants go (ROADMAP A6 follow-up).
- Sources sent over remote control may not read other files: no `#`
  directives, no `__include`, no `import "<path>"`, and `import` only names a
  library module (`rhi::check_material_source`).
- Test material sources live in `tests/data/materials/`;
  `tests/support/material_shader_tree.hpp` builds a scratch project with the
  library modules cooked.

## Cooking assets

`quat-cook <input.gltf|glb> <output.gltf|glb> [options]` (target
`quat-cook`) prepares a glTF for shipping. A `.glb` output is one binary
file; a `.gltf` output gets a `.bin` next to it. The input goes through the
runtime's own parse and validation (`parse_gltf`, `asset::decode_png_jpeg`
and its limits), so anything the cooker accepts the engine loads; already
cooked files can be cooked again (their meshopt streams are decoded first).
Cooked files target the engine: see the normal-map note below.

- **Textures** become KTX2 with Basis Universal supercompression
  (`KHR_texture_basisu`, the PNG/JPEG is dropped), full mip chains generated
  in linear light (normal maps renormalised), resized to multiples of 4 as
  the extension requires. Normal maps use UASTC (+ RDO + Zstd) and are stored
  as X in RGB / Y in alpha so they transcode to BC5; everything else uses
  ETC1S. Occlusion-only images are stored single-channel (BC4 at runtime).
  Third-party viewers read such a normal map's RGB as (X, X, X); textures in
  that layout carry `extras.quat_normal_xy: true` (the engine itself reads
  the layout from the KTX2 descriptor). Use `--normal-layout rgb` for files
  meant for other viewers.
- **Meshes**: primitives sharing a vertex stream set are optimised together
  (meshoptimizer vertex cache, overdraw, vertex fetch; unused vertices are
  dropped), then quantised — normals / tangents as 8-bit octahedral
  (`KHR_mesh_quantization`), UVs and colours in [0, 1] as 16-bit unorm,
  positions as floats through the exponent filter, skin weights as bytes —
  and compressed with the meshopt codecs (`EXT_meshopt_compression`, vertex
  codec v0 as the extension requires). Primitives with morph targets, or
  whose accessors other primitives share differently, are copied unchanged.
- Buffers are compacted to the data the output still references.
- **Level of detail** (`--simplify R`): before the passes above, each
  primitive is reduced to about R of its triangles with meshoptimizer's
  attribute-aware simplifier (normals and UVs weigh in; error bound
  `--simplify-error`, relative to the model's size). When the bound stops it
  above the target, the result is kept: the LOD misses its budget rather than
  change shape. Foliage made of
  thousands of disconnected cards or needle clusters cannot be simplified
  that way (small cards vanish within the error bound and the canopy
  empties), so a primitive of at least 64 position-welded components that
  average fewer than 24 triangles is thinned instead (stochastic
  simplification of aggregate detail, Cook et al. 2007), as foliage LODs usually are: a deterministic
  subset of whole components (cards, needle clusters) is kept and each grows
  about its centre by 1/sqrt(kept share), so the canopy keeps its projected
  area. (Vertex clustering merges neighbouring cards into large triangles
  whose UVs span unrelated texels; alpha-tested, those render as shards.)
  Vertices another primitive also uses are not moved. The cook writes one
  level; a manifest that wants several cooks several outputs (see
  `scripts/fetch-polyhaven.py`). Runtime LOD selection is not implemented yet.
- **Material rewrites** (`--blend-to-mask C`): scanned foliage is often
  exported as glTF BLEND, which the engine draws in the sorted forward pass
  without cut-out shadows; this turns every BLEND material into MASK with
  alpha cutoff C (before the textures are encoded, so the base colour gets
  coverage-preserving mips).

| Option | Default | |
|---|---|---|
| `--color-codec etc1s\|uastc` | `etc1s` | codec for every non-normal texture |
| `--normal-codec etc1s\|uastc` | `uastc` | codec for normal maps |
| `--normal-layout xy\|rgb` | `xy` | `rgb` keeps normals readable by other glTF viewers (BC7 instead of BC5) |
| `--etc1s-quality N` | 128 | 1..255 |
| `--uastc-level N` / `--uastc-rdo Q` / `--zstd N` | 2 / 1.0 / 18 | UASTC effort, RDO strength (0 = off), Zstd level (0 = off) |
| `--no-mips`, `--no-textures` | | |
| `--no-optimize`, `--no-quantize`, `--no-compress` | | skip the mesh passes |
| `--position-bits N`, `--normal-bits 8\|16` | 16, 8 | quantisation precision |
| `--threads N` | all cores | texture encoder threads |
| `--simplify R` / `--simplify-error E` | 1 / 0.01 | keep about R (0..1] of each primitive's triangles |
| `--blend-to-mask C` | off | BLEND materials become MASK with alpha cutoff C |

At load the engine picks the transcode target from what the device samples
(`rhi::TextureCaps`, enabled at device creation):

| Texture | BC (desktop, lavapipe) | ASTC only, or Apple GPU | Neither |
|---|---|---|---|
| Colour, sRGB or linear | BC7 | ASTC 4x4 | RGBA8 |
| Normal map, X/Y layout | BC5 | ASTC 4x4 + RA swizzle | RGBA8 + RA swizzle |
| Normal map, RGB layout | BC7 | ASTC 4x4 | RGBA8 |
| Single channel, linear | BC4 (+ RRR1 swizzle) | ASTC 4x4 | RGBA8 |

KTX2 files that already hold BC7/BC5/BC4/ASTC 4x4/RGBA8 data are uploaded
as they are when the device supports the format. Decoding and transcoding run
on worker threads (`extract_textures`); only the upload uses the device.

Every texture is untrusted input, checked against one configuration,
`asset::TextureLimits` (default 8192 per side, lowered to the device's
`maxImageDimension2D`; 512 MiB decoded per image; 2 GiB per load, shared by a
file's images through `asset::DecodeBudget`) before anything is allocated:

- PNG / JPEG (`asset::decode_png_jpeg`): only those two formats are compiled
  into stb_image (`STBI_MAX_DIMENSIONS` 16384 as a second guard); the size in
  the header is checked first and stb's buffer is adopted without a copy.
- KTX2 (`asset::validate_ktx2`): header, level index (in-file, ordered,
  sizes matching the extent), BasisLZ global data (codebook lengths, one
  image descriptor per level, slices inside their level), then libktx loads
  and transcodes. The `ktx` overlay port also makes libktx fail on corrupt
  BasisLZ codebooks instead of transcoding with them (a crash found by the
  fuzz test), and exports `KTX::ktx_read`, which the game links (no Basis /
  ASTC encoders; the `game_has_no_texture_encoder` CTest checks with `nm`).
  `QUAT_KTX2_FUZZ_ROUNDS=<n>` lengthens the `Ktx2Decode.CorruptedFilesNeverCrash`
  fuzz test, e.g. in an ASan build (CI runs it without sanitizers).
- `EXT_meshopt_compression` views are rejected when they claim more than
  1024x their compressed size or push the file past 1 GiB decoded.
Quantised attributes are dequantised to the float vertex layout at load —
smaller downloads, same VRAM per vertex; a quantised vertex format is a
later step.

## CI

`.github/workflows/ci.yml` runs on pushes to `master`, on pull requests and on
demand (`workflow_dispatch`):

| Job | Runner | Preset | Tests |
|---|---|---|---|
| `linux` | ubuntu-24.04 | `release` | CPU + GPU on Mesa lavapipe under the validation layer; then `game` once more with `ENGINE_EDITOR=OFF` (sharing the dependency tree) |
| `windows` | windows-2022 | `windows-release` | CPU + GPU on Mesa lavapipe (Windows build, from `jakoch/install-vulkan-sdk-action`) under the SDK's validation layer |
| `macos` | macos-15 (Apple Silicon) | `macos-release` | CPU; MSL translation check of every cooked shader; GPU on MoltenVK over the runner's paravirtual Metal device — both informational (`continue-on-error`) until P1 makes them a gate |
| `rust` | ubuntu-24.04 | — | `tools/quat-mcp` fmt, clippy, tests |

Each C++ job pins vcpkg to the manifest baseline and Slang to the same
release. The vcpkg binary cache (one per OS) is saved right after configure,
so only the first run (or a dependency change) pays for the full dependency
build (cold: ~17 min macOS, ~28 min Windows). Logs are uploaded as an artifact when
a run fails. The Windows and macOS test steps print GoogleTest's brief summary
(`GTEST_BRIEF=1`), so skipped GPU tests are visible in the log.

Gates and policies:

- **Warnings are errors** on every OS: CI configures with
  `-DENGINE_WERROR=ON`, which adds `-Werror` (`/WX` on MSVC) to
  `engine_flags`, i.e. first-party targets only. vcpkg ports, the
  FetchContent headers and the third-party translation units compiled with
  `ENGINE_THIRD_PARTY_TU_FLAGS` are unaffected. Local builds leave it off
  (a newer compiler's new warning should not block work); turn it on with
  the same flag before pushing. Fix a warning at its cause, never by
  silencing it.
- **Validation:** the Linux and Windows GPU tests run with
  `QUAT_VALIDATION=1` (see Testing): a validation error fails the test that
  caused it. macOS (MoltenVK, informational) runs without the layer.
- **Cancelling superseded runs:** runs share a concurrency group per pull
  request (`ci-pr-<N>`) or per ref. A new push to a PR cancels its running
  build. Pushes to `master`, manual runs and tags (`release.yml`) are never
  cancelled: a new push waits, and GitHub keeps only the newest pending run.
- **`publish-screenshots` has no concurrency group**, since a group cancels
  queued jobs, which dropped most PRs' publishes. Concurrent publishes are
  safe: each one commits on the tip it fetched and pushes without force,
  and a rejected push re-applies the run on the new tip (up to 12
  attempts, with jittered back-off).
- **Supply chain:** every workflow defaults to `permissions: contents:
  read`; only `publish-screenshots` (pushes `ci-screenshots`) and the
  release `publish` job have `contents: write`. Jobs that run build output
  or third-party build scripts check out with `persist-credentials: false`.
  Every third-party action is pinned to a full commit SHA (version in a
  comment) and `.github/dependabot.yml` proposes weekly updates. The Slang
  archive is checked against a pinned SHA-256 on every OS (update
  `slang-sha256-*` in `.github/actions/setup-build/action.yml` together
  with `slang-version`). The Windows Vulkan SDK action gets the job token
  (`github_token`), since anonymous GitHub API calls hit the runner IP's
  rate limit.

### Debugging MoltenVK

MoltenVK translates SPIR-V to Metal Shading Language with SPIRV-Cross when a
pipeline is created, and a failed translation surfaces only as
`vkCreateGraphicsPipelines failed: VkResult(<n>)` (issue #103). Two tools name
the shader and the error:

- **`scripts/msl-check.sh <cooked-shader-dir> [out-dir]`** translates every
  cooked entry point with SPIRV-Cross and, on macOS, compiles the MSL with
  `xcrun metal`. The `macos` job runs it after the build (informational). It
  also runs on Linux with `apt install spirv-cross`, but Ubuntu 24.04's
  package is from 2021 and both misses and invents errors compared with the
  current SPIRV-Cross; trust the macOS step (Homebrew's `spirv-cross`).
  Neither is the revision MoltenVK pins, so confirm a finding with
  `mvk_debug` (below), which shows MoltenVK's own translation.
- **`mvk_debug` workflow input:** `gh workflow run ci.yml --ref <branch> -f
  mvk_debug=true` runs the macOS GPU tests with `MVK_CONFIG_LOG_LEVEL=4`
  (debug), `MVK_CONFIG_DEBUG=1` (the generated MSL of each converted shader)
  and `MVK_CONFIG_SHADER_DUMP_DIR` (every SPIR-V and MSL shader MoltenVK
  saw), and uploads them as the `macos-mvk-debug` artifact. The same
  variables work on a Mac when running `engine_tests` by hand (MoltenVK's
  `Docs/MoltenVK_Configuration_Parameters.md` lists them all).

## Releases

`.github/workflows/release.yml` builds `game`, `quat-player` and `quat-pack` in Release with
`ENGINE_PACKAGE_BUILD=ON` on all three OSes (toolchain setup shared with CI
through `.github/actions/setup-build`), packages it with
`scripts/package-release.sh`, and smoke-tests each package with
`scripts/smoke-test-package.sh` (unpack, launch headless for 30 frames, require
a clean exit, a screenshot, and data resolved next to the executable):

| Package | Contents | Smoke test |
|---|---|---|
| `quat-3d-<v>-linux-x64.tar.gz` | `game` (RUNPATH `$ORIGIN/lib`), `lib/` libc++ runtime, data | Xvfb + lavapipe |
| `quat-3d-<v>-windows-x64.zip` | `game.exe`, vcpkg DLLs (applocal), MSVC runtime DLLs, data | lavapipe |
| `quat-3d-<v>-macos-arm64.zip` | `Quat3D.app`: Vulkan loader + MoltenVK in `Frameworks/` (ICD manifest in `Resources/vulkan/icd.d`), data in `Resources/`, ad-hoc signed | MoltenVK, with Homebrew's Vulkan uninstalled first |
| `coin-collector-<v>[-dev.<sha>]-<os>.{tar.gz,zip}` | The sample game through `quat-pack`: `coin-collector` / `coin-collector.exe` / `Coin Collector.app` (Info.plist and icon from `game.json`) next to `game.json`, `manifest.json` and only the data it uses | as above, plus the start scene must load |

Each archive carries `packaging/README-RUN-<os>.txt` as `README-RUN.txt`.
Pushing a `v*` tag publishes a GitHub Release (tags with a `-`, e.g.
`v0.2.0-rc1`, are prereleases); running the workflow by hand
(`gh workflow run release.yml --ref <branch>`) only uploads the packages as
workflow artifacts.

## Game projects and packaging

Games live in `games/<name>/` with a `game.json` (docs/game-packaging.md):

```sh
QUAT_GAME=games/coin-collector ./build/release/game          # edit it in the editor
QUAT_GAME=games/coin-collector ./build/release/quat-player   # play it, no editor
./build/release/quat-pack games/coin-collector --player ./build/release/quat-player \
    --out dist [--config development] [--smoke-test] [--stage-only]
```

`quat-pack` validates `game.json`, collects the files the game references
(the start scene and scenes in `content` roots → models, audio, scripts; Lua
`require`s and script literals), cooks models with `quat-cook` when it is
next to `quat-pack` or on `PATH` (R17), stages the data root with
`manifest.json`, and lays out the package with
`scripts/package-release.sh` (`--stage-only` stops after staging). Progress
lines (`[pack] step i/n ...`, `[pack] package <archive>`) drive the editor's
*File → Build & Export…* dialog and the remote `project.build` /
`project.build_status` methods, which run it in the background. Development
builds of `quat-pack` find the cooked shaders and scripts in the source
tree; package builds take `--shaders` and `--engine-root` (or
`QUAT_SHADER_DIR` / `QUAT_ENGINE_ROOT`). On Windows the layout runs under
Git Bash (`QUAT_BASH` overrides where it is).

The player resolves its game from `QUAT_GAME`, else `game.json` in the data
root next to the executable; it changes the working directory to the game
project so scene-relative paths resolve the same everywhere. Shipping
packages contain `quat-player-shipping`, which has no remote control at all;
`quat-player` honours `QUAT_REMOTE` only outside a package or in a package
whose `manifest.json` says `development` (missing or invalid: shipping), and
`QUAT_GAME` cannot raise that. `quat-pack --out` / `project.build` `output`
may not equal, lie inside or contain a content root or the start scene's
folder.
### Screenshots

After the tests (also when they fail), the `linux` job renders a fixed set of
scenes with the real `game` binary on lavapipe under Xvfb —
`scripts/ci-screenshots.sh`: the showcase with and without the editor UI,
`QUAT_TERRAIN=1`, `QUAT_PHYSICS=1`, the mountain-forest showcase
(`QUAT_DEMO=mountain`), an HDRI-lit showcase (`QUAT_HDRI`, the
CC0 panorama fetched by `scripts/fetch-hdri.sh`), the world clock at sunrise, solar
noon and sunset (`QUAT_TIME`, frozen) and the editor's world-clock Inspector, each with a frame budget, muted audio
and a per-scene timeout (`SCENE_TIMEOUT`, default 300 s; a scene row can raise its
own) so a hang fails that scene instead of the job. Each final frame is converted to PNG
(`scripts/ppm_to_png.py`, halved when 1280 px or wider, except the editor capture, kept full size so its UI stays readable). A crash, timeout or
missing image is listed in the step summary with the log tail and an
`::error` annotation. The step is `continue-on-error` for now (temporary,
until the headless scenes have proven stable). Rendered captures are
checked by `scripts/check_capture.py`: no large exact-black rectangle in any
scene (a NaN/Inf spreading through the HDR chain), and the editor scene must
show the docked editor UI; a failed check fails the job (`Capture checks`
step).

Where the images land:

- the `linux-screenshots` artifact (PNGs, per-scene logs, `results.tsv`);
- for pushes and same-repository PRs, `scripts/ci-publish-screenshots.sh`
  commits them to the **`ci-screenshots` branch** under
  `runs/<pr-N or branch>/<short-sha>/` and rebuilds `index.md` there (newest
  first, images inline), then shows them inline in the step summary. Commits
  are append-only (no force-push; a lost push race re-applies on the new
  tip). Fork PRs never publish. Publishing runs in its own
  `publish-screenshots` job (the only one with a write token; the build job
  is read-only), from the uploaded artifact. Retention: each publish keeps
  the newest 10 runs per PR/branch and the newest 20 of `master`
  (`KEEP_RUNS`, `KEEP_DEFAULT_RUNS`) at the branch tip; older run
  directories are deleted there but stay in the branch history. When the
  branch grows too large, re-root it by hand — the only force-push allowed,
  on this machine-owned branch only: check out `ci-screenshots`, `git
  checkout --orphan tmp && git commit -m "Re-root ci-screenshots"`, then
  `git push --force origin tmp:ci-screenshots`. To read them without the browser:
  `git fetch origin ci-screenshots && git show origin/ci-screenshots:index.md`.

Scenes built from downloaded CC0 assets (the `cinematic` row) need them
fetched first: before the Screenshots step, CI runs
`scripts/fetch-polyhaven.py` on every `assets/showcase/*/assets.json` with
the build's `quat-cook`, caching the result (`actions/cache`, keyed on the
manifests, the fetch script and the cooker sources). Fetching is
`continue-on-error` with a 25-minute timeout, and its downloads and cooked models are cached separately; a row whose manifest is not satisfied is reported as
*skipped (assets not fetched)*, not as a failure. See
[`cinematic-showcase.md`](cinematic-showcase.md).

Run locally: `scripts/ci-screenshots.sh build/release/game /tmp/shots`
(`SCENES="showcase physics"` picks a subset; needs `xvfb` and
`mesa-vulkan-drivers`).

## Generated assets

The nature models in `assets/meshes/nature/` (conifers and their
distant-forest variants, broadleaf trees, rocks, a bush) are generated, not
authored: `tools/asset-gen` builds them procedurally from a seed and writes
glTF 2.0 binaries. The `quat-asset-gen` tool is not part of the default
build (`EXCLUDE_FROM_ALL`; its library is built for `engine_tests`); after
changing the generator, build it explicitly and regenerate:

```sh
cmake --build --preset release --target quat-asset-gen
./build/release/tools/asset-gen/quat-asset-gen assets/meshes/nature   # --seed N (default 1)
```

`AssetGen.CommittedAssetsAreCurrent` fails while the committed files are
stale. The mountain showcase (`assets/scenes/mountain_forest.json`) scatters
them; `MountainScene.*` checks its instance counts and that every placement
respects its layer's slope / height limits.

## Building in a cloud container

Claude Code cloud sessions start without the toolchain. Use the `quat-3d`
cloud environment (it allows `codeload.github.com`, which vcpkg needs; the
default environment blocks it), then:

```sh
# toolchain + SDL3 system headers
apt-get update
apt-get install -y libc++-18-dev libc++abi-18-dev pkg-config libvulkan-dev \
  libx11-dev libxext-dev libxft-dev libxcursor-dev libxrandr-dev libxi-dev \
  libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libegl-dev libdecor-0-dev autoconf automake libtool

# vcpkg at the manifest baseline
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
git -C ~/vcpkg checkout 10ceb139a610ebf3c6aa49cdc4a4b7f3db5d3f2b
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# slangc (GitHub release downloads are reachable), checked against the
# SHA-256 that .github/actions/setup-build/action.yml pins
curl -fsSL -o /tmp/slang.tgz https://github.com/shader-slang/slang/releases/download/v2025.6.1/slang-2025.6.1-linux-x86_64.tar.gz
echo "836c8a23b5619dcfec13676c924ddadb46d8dd5e82cae2cad26d3a28da4d7538  /tmp/slang.tgz" | sha256sum -c -
mkdir -p ~/slang && tar xzf /tmp/slang.tgz -C ~/slang
export PATH="$HOME/slang/bin:$PATH"

cmake --preset release -DENGINE_PROFILE=OFF   # first run builds all dependencies (long)
cmake --build --preset release
VK_ICD_FILENAMES=/opt/lavapipe/lvp_icd.json ctest --test-dir build/release --output-on-failure
```

Configure also fetches ACL v2.1.0 and RTM v2.2.0 (header-only animation
compression, `cmake/FetchAcl.cmake`) with git from GitHub. Offline, pass
`-DFETCHCONTENT_SOURCE_DIR_ACL=<acl clone> -DFETCHCONTENT_SOURCE_DIR_RTM=<rtm clone>`.

### Compile-checking without vcpkg

When vcpkg can't download (blocked network), clone the dependency headers at
the versions vcpkg pins (`grep '"version' ~/vcpkg/ports/<port>/vcpkg.json`) and
syntax-check translation units with the engine's warning flags:

```sh
mkdir -p ~/deps && cd ~/deps
git clone --depth 1 --branch v3.16.0 https://github.com/skypjack/entt.git
git clone --depth 1 --branch v3.4.0  https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git vma
git clone --depth 1 --branch v5.5.0  https://github.com/jrouwe/JoltPhysics.git jolt
git clone --depth 1 --branch v0.9.0  https://github.com/spnda/fastgltf.git
git clone --depth 1 https://github.com/nothings/stb.git
git clone --depth 1 --branch release-3.4.10 https://github.com/libsdl-org/SDL.git sdl
git clone --depth 1 --branch 0.11.25 https://github.com/mackron/miniaudio.git
git clone --depth 1 --branch v1.92.8-docking https://github.com/ocornut/imgui.git
# imgui-node-editor (Behaviour panel): the vcpkg port's tag plus its patches,
# which make 0.9.3 build against imgui 1.92
git clone --depth 1 --branch v0.9.3 https://github.com/thedmd/imgui-node-editor.git
GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch v4.4.2 https://github.com/KhronosGroup/KTX-Software.git ktx
git clone --depth 1 --branch v1.1.1 https://github.com/zeux/meshoptimizer.git
for p in fix-vec2-math-operators remove-getkeyindex fix-imgui-v1.92.5; do
  patch -d imgui-node-editor -p1 < ~/vcpkg/ports/imgui-node-editor/$p.patch; done
# ACL + RTM (animation compression; not in vcpkg — cmake/FetchAcl.cmake pins them)
git clone --depth 1 --branch v2.1.0 https://github.com/nfrechette/acl.git
git clone --depth 1 --branch v2.2.0 https://github.com/nfrechette/rtm.git
apt-get install -y nlohmann-json3-dev libglm-dev libvulkan-dev

D=~/deps; cd ~/quat-3d
clang++ -std=c++23 -stdlib=libc++ -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti -fsyntax-only \
  -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_RADIANS -DENGINE_EDITOR \
  -DQUAT_COOKED_SHADER_DIR='"x"' -DQUAT_PROJECT_ROOT='"x"' -Iinclude -Isrc -Isrc/remote \
  -isystem $D/entt/src -isystem $D/vma/include -isystem $D/jolt -isystem $D/fastgltf/include \
  -isystem $D/stb -isystem $D/sdl/include  src/scene/scene.cpp
```

Lua and sol2 (engine_script, G7) are not in that list: clone sol2 v3.5.0
(header-only) and Lua v5.4.8 into a scratch directory and pass both through
`EXTRA_FLAGS`. Lua builds into a static library with one compiler call per
file, so the script tests can be linked and run locally:

```sh
git clone --depth 1 --branch v3.5.0 https://github.com/ThePhD/sol2.git $S/sol2
git clone --depth 1 --branch v5.4.8 https://github.com/lua/lua.git $S/lua
(cd $S/lua && for f in l*.c; do case $f in lua.c|ltests.c) continue;; esac
   cc -O2 -DLUA_USE_POSIX -c $f; done && ar rcs liblua.a l*.o)
EXTRA_FLAGS="-isystem $S/sol2/include -isystem $S/lua" scripts/check-compile.sh src/script/*.cpp
```

Both profiling configurations must compile: the default above is
`ENGINE_PROFILE=OFF`. For `ON`, clone Tracy at the vcpkg version
(`git clone --depth 1 --branch v0.13.1 https://github.com/wolfpld/tracy.git`)
and add `EXTRA_FLAGS="-DQUAT_TRACY -DTRACY_ENABLE -DTRACY_ON_DEMAND -isystem
<tracy>/public"`.

This catches compile errors and warnings, not link errors or test failures.
`scripts/check-compile.sh <files...>` wraps exactly this (set `DEPS` if the
clones live elsewhere than `~/deps`). Files under `src/app/` and
`src/player/` are checked three times — the editor build, `quat-player`
(no `ENGINE_EDITOR`) and `quat-player-shipping` (`QUAT_SHIPPING`) — the
last two without the source-tree path macros. Also needed for the whole tree:
ImGuizmo (`-isystem <clone>/src`), nlohmann-json, glm and the Vulkan headers
(through `EXTRA_FLAGS` when not installed), and libc++ (`apt-get install
libc++-dev libc++abi-dev`).

The `quat_player_shipping_has_no_remote` check (`cmake/CheckNoRemoteControl.cmake`)
can be approximated without a link: compile the shipping player's app-layer
sources (`QUAT_SHIPPING_APP_SOURCES` with `-DQUAT_SHIPPING`, plus
`game_world_runtime`'s sources without it) with `-c -O2` and run its pattern
over `nm -C` of the objects. Data types in `engine::remote::` count: remote-only
wiring (screenshot cameras, `terrain.status`) belongs in code compiled per
executable, guarded by `QUAT_SHIPPING`, not in the shared world library.

Modules whose dependencies are header-only (animation and terrain need glm;
remote needs nlohmann-json and glm — `apt-get install nlohmann-json3-dev
libglm-dev`) can also be linked and their tests run, against a GoogleTest built
with libc++ in `~/deps/googletest/build`:

```sh
G=~/deps/googletest
clang++ -std=c++23 -stdlib=libc++ -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti \
  -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_RADIANS -Iinclude -Isrc -Itests \
  -isystem $G/googletest/include src/remote/*.cpp tests/remote/*_test.cpp \
  $G/build/lib/libgtest_main.a $G/build/lib/libgtest.a -lpthread -o /tmp/remote_tests
/tmp/remote_tests
```

## Pitfalls

- **No exceptions:** the engine builds with `-fno-exceptions`, so
  `nlohmann::json::get<T>()` / `.value()` on a wrongly typed field aborts the
  process. Read files and network input through `engine/core/json_fields.hpp`.
  Dump JSON that may contain non-UTF-8 strings with
  `error_handler_t::replace`.
- **libc++ everywhere (Linux):** every vcpkg port with compiled C++ must use
  the `x64-linux-libcxx` triplet (see `triplets/`), or linking fails. macOS
  uses `arm64-osx-14` only to pin the minimum OS (Apple clang defaults to
  libc++); Windows uses MSVC's STL throughout.
- **MSVC builds at `/W4` with no warnings.** Third-party headers must come in
  through SYSTEM include directories (`/external:I`, warning level 0) — use
  `target_include_directories(<t> SYSTEM ...)` for any non-imported
  third-party path (e.g. stb).
- **Main-thread ECS:** systems, editor actions and remote-control handlers all
  mutate the registry on the main thread; do not add threads that touch it.
