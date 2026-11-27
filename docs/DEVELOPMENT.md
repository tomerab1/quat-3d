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
`engine_nav`, `engine_net`, `engine_remote`, `engine_editor`), the `game`
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
| `tests/legacy/` | Wrappers around the old `run_*_self_test()` functions, removed as their bodies migrate into `tests/<module>/` |


## CI

`.github/workflows/ci.yml` builds the `release` preset on Ubuntu 24.04 and runs
`ctest` with lavapipe. The vcpkg binary cache is saved right after configure,
so only the first run (or a dependency change) pays for the full dependency
build. Logs are uploaded as an artifact when a run fails.

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

Quick check without vcpkg (the remote-control library only needs
nlohmann-json and glm headers — `apt-get install nlohmann-json3-dev libglm-dev`):

```sh
clang++ -std=c++23 -stdlib=libc++ -Wall -Wextra -Wshadow -fno-exceptions -fno-rtti \
  -Iinclude -Isrc/remote src/remote/*.cpp <a main() calling run_rpc_server_self_test()>
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
