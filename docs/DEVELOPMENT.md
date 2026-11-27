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
executable, the offline cooker (`engine_cook` + the `quat-cook` CLI, never
linked into the game) and the GoogleTest suite `engine_tests` (`tests/`).

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

`.github/workflows/ci.yml` builds the `release` preset on Ubuntu 24.04 and runs
`ctest` with lavapipe, then builds the `game` target once more with
`ENGINE_EDITOR=OFF` (sharing the dependency tree). The vcpkg binary cache is saved right after configure,
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
