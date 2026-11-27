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
| `release` | clang + libc++, optimised (Tracy enabled via `ENGINE_PROFILE`) |
| `windows-release` | MSVC |

CMake options: `ENGINE_EDITOR` (default ON), `ENGINE_PROFILE`,
`ENGINE_DESCRIPTOR_SETS_FALLBACK` (macOS / drivers without descriptor buffers).

Targets: the engine libraries (`engine_rhi`, `engine_renderer`,
`engine_scene`, `engine_physics`, `engine_animation`, `engine_terrain`,
`engine_nav`, `engine_net`, `engine_remote`, `engine_audio`, `engine_editor`), the `game`
executable, and the GoogleTest suite `engine_tests` (`tests/`).

## Testing

```sh
ctest --test-dir build/release --output-on-failure
./build/release/engine_tests --gtest_list_tests       # every test
./build/release/engine_tests --gtest_filter='-Gpu*'    # device-free tests only
./build/release/engine_tests --gtest_filter='Nav.*:Ai.*'
./build/release/engine_tests --validation              # GPU tests under the validation layer
./build/release/engine_tests --require-gpu             # fail (not skip) GPU tests without a device
```

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

## CI

`.github/workflows/ci.yml` builds the `release` preset on Ubuntu 24.04 and runs
`ctest` with lavapipe, then builds the `game` target once more with
`ENGINE_EDITOR=OFF` (sharing the dependency tree). The vcpkg binary cache is saved right after configure,
so only the first run (or a dependency change) pays for the full dependency
build. Logs are uploaded as an artifact when a run fails.

### Screenshots

After the tests (also when they fail), the `linux` job renders a fixed set of
scenes with the real `game` binary on lavapipe under Xvfb —
`scripts/ci-screenshots.sh`: the showcase with and without the editor UI,
`QUAT_TERRAIN=1` and `QUAT_PHYSICS=1`, each with a frame budget, muted audio
and a per-scene timeout (`SCENE_TIMEOUT`, default 300 s) so a hang fails that
scene instead of the job. Each final frame is converted to PNG
(`scripts/ppm_to_png.py`, halved when 1280 px or wider). A crash, timeout or
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

Run locally: `scripts/ci-screenshots.sh build/release/game /tmp/shots`
(`SCENES="showcase physics"` picks a subset; needs `xvfb` and
`mesa-vulkan-drivers`).

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

# slangc (GitHub release downloads are reachable)
mkdir -p ~/slang && curl -sSL https://github.com/shader-slang/slang/releases/download/v2025.6.1/slang-2025.6.1-linux-x86_64.tar.gz | tar xz -C ~/slang
export PATH="$HOME/slang/bin:$PATH"

cmake --preset release -DENGINE_PROFILE=OFF   # first run builds all dependencies (long)
cmake --build --preset release
VK_ICD_FILENAMES=/opt/lavapipe/lvp_icd.json ctest --test-dir build/release --output-on-failure
```

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
for p in fix-vec2-math-operators remove-getkeyindex fix-imgui-v1.92.5; do
  patch -d imgui-node-editor -p1 < ~/vcpkg/ports/imgui-node-editor/$p.patch; done
apt-get install -y nlohmann-json3-dev libglm-dev libvulkan-dev

D=~/deps; cd ~/quat-3d
clang++ -std=c++23 -stdlib=libc++ -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti -fsyntax-only \
  -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_FORCE_RADIANS -DENGINE_EDITOR \
  -DQUAT_COOKED_SHADER_DIR='"x"' -DQUAT_PROJECT_ROOT='"x"' -Iinclude -Isrc -Isrc/remote \
  -isystem $D/entt/src -isystem $D/vma/include -isystem $D/jolt -isystem $D/fastgltf/include \
  -isystem $D/stb -isystem $D/sdl/include  src/scene/scene.cpp
```

This catches compile errors and warnings, not link errors or test failures.
`scripts/check-compile.sh <files...>` wraps exactly this (set `DEPS` if the
clones live elsewhere than `~/deps`).

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
- **libc++ everywhere:** every vcpkg port with compiled C++ must use the
  `x64-linux-libcxx` triplet (see `triplets/`), or linking fails.
- **Main-thread ECS:** systems, editor actions and remote-control handlers all
  mutate the registry on the main thread; do not add threads that touch it.
