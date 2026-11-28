# The quat-3d engine SDK

A game in its own repository builds against an **installed, versioned engine
SDK**, not against the engine's sources (ROADMAP P8; design in
[`design/cozy-builder.md` §3.3–3.5](design/cozy-builder.md#33-how-the-game-consumes-the-engine-an-installed-sdk-package-p8)).
The SDK is a CMake package: `find_package(quat3d 0.N CONFIG)` plus the
exported `quat3d::` targets.

## Getting an SDK

Each release attaches one archive per OS:

| Archive | Toolchain a game must use |
|---|---|
| `quat3d-sdk-<version>-linux-x64.tar.gz` | clang (same major version) with libc++ |
| `quat3d-sdk-<version>-windows-x64.zip` | MSVC v14x, at least the SDK's version |
| `quat3d-sdk-<version>-macos-arm64.tar.gz` | Apple clang (same major version), macOS 14+ |

You can also build one from a checkout:

```sh
cmake --preset release -DENGINE_PACKAGE_BUILD=ON
cmake --build --preset release
(cd tools/quat-mcp && cargo build --release --locked)   # optional: bin/quat-mcp
cmake --install build/release --prefix ~/sdk/quat3d-0.1
# or, archived exactly as the release workflow does:
scripts/package-sdk.sh build/release dist v0.1.0
```

Use `ENGINE_PACKAGE_BUILD=ON` for an SDK you hand to others. Without it,
`engine_rhi` embeds this checkout's slangc path for shader hot reload (the
development data-root fallback is compiled into the engine's own `game`
executable only, not into any SDK library). The CMake package files are
relocatable either way. `sdk_install` checks that they name neither the source
tree nor the build tree.

## Prerequisites

- **Vulkan headers and loader**: the one link dependency the SDK does not
  bundle in `deps/`. Install the Vulkan SDK on Windows and macOS (or set
  `VULKAN_SDK`), or `libvulkan-dev` / your distribution's loader package on
  Linux. `find_package(quat3d)` names this when it is missing.
- CMake 3.22+ and Ninja (or Visual Studio's generator on Windows), and the
  toolchain listed above.

## Layout

| Path | Content |
|---|---|
| `include/engine/**` | the installed headers (see [What is stable](#what-is-stable)) and the generated `engine/version.hpp` |
| `lib/` | the engine static libraries, and Slang's shared libraries on Linux/macOS |
| `lib/cmake/quat3d/` | `quat3dConfig.cmake`, `quat3dConfigVersion.cmake`, `quat3dTargets*.cmake`, `quat3d-toolchain.cmake`, `Quat3dGame.cmake`, `Quat3dToolchainCheck.cmake` |
| `bin/` | `quat-cook`, `quat-pack`, `quat-player` and `quat-player-shipping` (P4: run and package a data-plus-Lua game), `slangc` (plus Slang's DLLs on Windows), `quat-mcp` (release archives), and the dependency DLLs on Windows |
| `deps/` | the release half of the vcpkg tree the engine was built with: every library the engine links, plus GoogleTest. A game never runs vcpkg |
| `share/quat3d/` | a data root: `cooked/shaders/` (the engine's shaders), `assets/shaders/lib/*.slang` (the modules material shaders import), `assets/fonts/` (the editor's fonts and icons), plus licences. It is also `quat-pack`'s engine root (`scripts/package-release.sh`, `scripts/smoke-test-package.sh`, `packaging/`), which `bin/quat-pack` finds on its own |

## Using it

```cmake
cmake_minimum_required(VERSION 3.22)
project(my_game LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(quat3d 0.1 CONFIG REQUIRED)

quat_add_game(my_game
    SOURCES src/main.cpp src/my_systems.cpp
    ASSETS assets              # copied to <exe dir>/assets
    MATERIALS shaders/materials  # authored material shaders, cooked with the SDK's slangc
    REMOTE)                    # + quat3d::remote (JSON-RPC endpoint and world commands)
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<sdk>/lib/cmake/quat3d/quat3d-toolchain.cmake
cmake --build build
```

The toolchain file does four things:
- selects the SDK's compiler family (clang with `-stdlib=libc++` on Linux,
  `cl` with `/MD` on Windows, and the deployment target on macOS);
- puts `<sdk>` and `<sdk>/deps` on `CMAKE_PREFIX_PATH`, so
  `find_package(GTest)` and the other bundled dependencies resolve too;
- maps every build configuration onto the SDK's release libraries;
- includes your own toolchain file last, if you name it in
  `QUAT3D_CHAINLOAD_TOOLCHAIN_FILE`.

Targets:

| Target | What it is |
|---|---|
| `quat3d::engine` | the engine libraries: core, scene/ECS, physics, script, terrain, navigation, networking, audio, asset, renderer |
| `quat3d::remote` | `quat3d::engine` plus the remote-control endpoint and world commands |
| `quat3d::editor` | the ImGui editor layer's libraries (SDKs built with `ENGINE_EDITOR=ON`); no editor headers are installed: a game reaches the editor through `quat3d::player_editor` |
| `quat3d::player`, `quat3d::player_shipping`, `quat3d::player_editor` | the application layer behind `engine::game::run_player` / `run_editor` (`engine/game/player.hpp`): what a game with native modules links its executables against (G17.1; `quat_add_game(... MODULES)` does it) |

`quat3d::engine` carries the settings the headers depend on: C++23,
`GLM_FORCE_DEPTH_ZERO_TO_ONE` / `GLM_FORCE_RADIANS`, no exceptions, no RTTI,
and `ENGINE_EDITOR` when the SDK was built with it. Warning flags are not
passed on to your code.

`quat_add_game(<target> SOURCES … [ASSETS dir] [MATERIALS dir] [LIBRARIES …] [REMOTE] [EDITOR])`
makes the executable's directory a data root, the layout the engine reads its
data from (`assets/` next to `cooked/shaders/`):
- the SDK's `cooked/shaders/`;
- your `MATERIALS` cooked into `cooked/shaders/materials/`;
- the SDK's `assets/fonts/`;
- your `ASSETS`.

On Windows it also copies the dependency DLLs; call
`quat_copy_runtime_dlls(<target>)` to do the same for your test executables.
Native game modules (G17.1): `quat_add_game_module(<target> SOURCES …
REGISTER <ns::fn>)` makes a module library, and
`quat_add_game(<target> PROJECT_DIR <dir> MODULES <module>…)` builds the game's
player, shipping player and editor with a generated `main()` that registers
them. See [`game-modules.md`](game-modules.md).

Package variables:
- `QUAT3D_SDK_ROOT`;
- `QUAT3D_DATA_DIR` (`share/quat3d`);
- `QUAT3D_SLANGC`, `QUAT3D_COOK`, `QUAT3D_MCP`;
- `quat3d_VERSION`.

A data-plus-Lua game (`game.json`, scenes, scripts; see
[`game-packaging.md`](game-packaging.md)) needs no C++ at all: run it with
`QUAT_GAME=<game> <sdk>/bin/quat-player` and package it with
`<sdk>/bin/quat-pack <game> --player <sdk>/bin/quat-player --out dist`.
`quat-pack` takes its engine root and cooked shaders from `<sdk>/share/quat3d`
(`QUAT_ENGINE_ROOT` / `QUAT_SHADER_DIR` or `--engine-root` / `--shaders`
override them).

[`examples/sdk-consumer/`](../examples/sdk-consumer) is a complete, tested
example: a component, a system, a remote method, a save chunk, a physics body,
a native module with its own players, and GoogleTest from the SDK's `deps/`.

### Toolchain

The SDK is static C++, and C++ has no ABI across compilers or standard
libraries. `quat3dConfig.cmake` therefore refuses a mismatched toolchain at
configure time, with a message that names the expected compiler and the
toolchain file. It does not wait for the link to fail with undefined symbols.
The rule (`cmake/Quat3dToolchainCheck.cmake`):
- the compiler ID must match;
- clang and Apple clang must have the same major version;
- MSVC must be the same v14x family and at least as new as the SDK's;
- the standard library must match (libc++ on Linux is checked with a compile
  test, so it does not matter how it was selected).

`-DQUAT3D_ALLOW_TOOLCHAIN_MISMATCH=ON` turns the refusal into a warning.
That is unsupported.

### Engine source override (co-development only)

A game can build the engine from a checkout instead: `add_subdirectory()` it,
for example behind a `QUAT3D_SOURCE_DIR` option in the game's top-level CMake,
in place of `find_package`. The engine defines the same target names
(`quat3d::engine`, `quat3d::remote`, `quat3d::editor` as ALIASes) and the same
`quat_add_game()`. A sub-project build installs nothing (`QUAT3D_INSTALL`
defaults to OFF there). Game CI never uses this mode.

## What is stable

Installed headers (`cmake/Quat3dSdkHeaders.cmake`):

| Set | Headers |
|---|---|
| **Stable API** | `engine/core/{error, json_fields, log, path_confine, sim_clock, spring, utf8_path}.hpp` · `engine/game/{module, fields, module_set, json_schema, player}.hpp` (G17.1) · `engine/input/{actions, action_map, device_state}.hpp` · `engine/scene/{components, scene, scene_io, component_json, component_registry, field_table, scatter, ground_placement, camera_framing, camera_rig, navigation}.hpp` · `engine/script/module_bind.hpp` · `engine/physics/physics_world.hpp` · `engine/project/game_project.hpp` · `engine/terrain/heightmap.hpp` · `engine/remote/rpc_server.hpp` · `engine/version.hpp` |
| **Support** (the stable headers include them; same versioning) | `engine/animation/{animator, clip, foot_placement, inertialization, skeleton, two_bone_ik}.hpp` · `engine/asset/{asset_manager, material_params}.hpp` · `engine/audio/audio_types.hpp` · `engine/nav/navmesh.hpp` · `engine/renderer/{grass, particles, physical_camera}.hpp` (settings structs) · `engine/scene/animation_system.hpp` · `engine/terrain/generator.hpp` |

Not installed, and not API:
- `engine/rhi/*` and the renderer passes;
- `engine/asset/{mesh,material,texture}_asset.hpp` (they own GPU resources);
- `engine/cook/*` and the `engine/net/*` internals;
- the editor, and everything under `src/`.

The installed headers contain no Vulkan, VMA or other graphics-API includes.
`components.hpp` names `MeshAsset`/`MaterialAsset` only through
`AssetHandle` (`valid()` works on those handles). Games reach rendering
through components and data. If a game needs more, a minimal Vulkan-free
interface is promoted into the stable set. The pass header is never shipped.
`engine/scene/instanced_mesh.hpp` is the first candidate: it still exposes
renderer types.

Every installed header compiles on its own against the installed tree, with
only glm, EnTT and nlohmann_json (the SDK's public-header dependencies) and
warnings as errors. `public_headers_self_contained` enforces that.

## Versioning and deprecation

[Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html), from
`v0.1.0` on.

- **0.x**
  - **PATCH** releases never break the stable API, the data formats or the
    remote protocol.
  - **MINOR** releases may break the API. Each break is listed in
    `CHANGELOG.md` under "Breaking", with a migration note.
  - Where feasible, the old name lives on as
    `[[deprecated("use X; removed in 0.(N+1)")]]` for one minor release.
  - `quat3dConfigVersion.cmake` uses **SameMinorVersion**:
    `find_package(quat3d 0.4)` accepts only 0.4.x. Under 0.x a minor
    release is the unit of API change, so accepting 0.5 for a game written
    against 0.4 would trade a clear configure-time refusal for compile or
    link errors (`sdk_config_refusal_version` tests that refusal).
- **From 1.0**
  - breaks happen only in MAJOR releases;
  - deprecated APIs stay for at least one MINOR release and go in the next
    MAJOR;
  - **SameMajorVersion** (cmake/Quat3dInstall.cmake switches automatically).
- **ABI**: none is promised. Games relink against each release (static
  linking makes that the normal workflow). A plugin ABI is a non-goal.
- **Version queries**: the `QUAT3D_VERSION_{MAJOR,MINOR,PATCH,STRING}` macros
  give the headers' version, `quat3d::version()` and
  `quat3d::version_string()` the linked libraries' version.
- **Data formats** (scene files, `game.json`, saves) are versioned separately
  and read backwards. A newer format is refused with "needs engine ≥ X".
- **Remote / MCP protocol**: method names are stable within a minor release.
  Removals go through a deprecation that `rpc.methods` reports.

## Guards in the engine repository

CTest label `sdk` (`tests/sdk/`). The Linux CI job runs it with the full
suite, Windows and macOS run `ctest -L sdk`, and the release workflow runs it
before uploading each SDK archive.

| Test | Checks |
|---|---|
| `sdk_install` | installs the build into a fresh prefix (the real layout); required files present; no private headers; no build-machine paths in package files or headers |
| `public_headers_self_contained` | one TU per installed header, compiled against the installed tree alone, `-Werror` / `/WX` |
| `sdk_consumer` | `examples/sdk-consumer/` configured, built and tested against the prefix with `find_package(quat3d 0.N)` and the toolchain file only |
| `sdk_config_refusal_version` | `find_package(quat3d 0.<N+1>)` is refused |
| `sdk_config_refusal_compiler` | Linux: configuring with GCC is refused with the toolchain message |
| `sdk_toolchain_rules` | the compiler-match rule, in script mode |
| `layering_check` | nothing under `src/ include/ tools/ tests/ assets/ cmake/` refers to `games/` or `examples/` (includes, Lua requires, any CMake path token); installed headers include only installed headers (`cmake/CheckLayering.cmake`). Its link-graph half runs at configure time: no engine target links a non-engine local target, by name or inside a generator expression (`cmake/CheckLinkLayering.cmake`) |
| `layering_check_detects_violation` | both halves fail on fixture violations (`tests/sdk/layering_fixture/`) |

Locally: `ctest --test-dir build/release -L sdk --output-on-failure`.
