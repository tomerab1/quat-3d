# quat-3d

A modular, data-driven C++23 game engine with a Vulkan 1.3 renderer, Slang shaders,
glTF 2.0 skeletal animation, Jolt physics, and an ImGui-based editor.

## Features

**Rendering** (Vulkan 1.3, dynamic rendering + descriptor buffers)
- Deferred shading with cascaded shadow maps
- Physically-based sky (Hillaire LUTs) with volumetric clouds
- Image-based lighting with an incrementally refreshed dynamic probe
- TAA, bloom, histogram auto-exposure, filmic tonemapping
- Frame-graph driven: passes declare reads/writes, barriers are derived

**Animation**
- glTF 2.0 skeletal animation via fastgltf
- GPU skinning (compute pass, not vertex-shader skinning)
- Blend trees (1D blends, additive layers) and a state machine with crossfades

**Physics**
- Jolt Physics integration (fixed 1/120 s step), collider shapes honour entity scale
- Editor debug-draw overlay

**Scene / ECS**
- EnTT-based scene with parent/child hierarchy
- glTF import (meshes, materials, skins, clips) through a ref-counted asset manager
- Scene save/load to JSON

**Editor** (ImGui docking)
- Scene hierarchy, inspector, asset browser, animation preview, physics debug
- ImGuizmo transform gizmos, play/stop with scene snapshot restore

## Tech stack

| Domain      | Library                                   |
|-------------|-------------------------------------------|
| Graphics    | Vulkan 1.3 (dynamic rendering, descriptor buffer, sync2) |
| Shaders     | Slang, cooked offline to SPIR-V + reflection JSON |
| Windowing   | SDL3                                      |
| Physics     | Jolt Physics                              |
| ECS         | EnTT                                      |
| glTF        | fastgltf                                  |
| Math        | glm (depth 0..1, radians)                 |
| UI          | Dear ImGui (docking) + ImGuizmo           |
| Allocation  | Vulkan Memory Allocator                   |
| Build       | CMake + vcpkg (manifest mode)             |

## Requirements

To **run** the engine you need a Vulkan 1.3 driver with `dynamicRendering`,
`synchronization2`, and `bufferDeviceAddress`, plus either:

- `VK_EXT_descriptor_buffer` (any recent NVIDIA / AMD / Intel driver, or Mesa
  RADV/ANV/lavapipe), **or**
- a build with `ENGINE_DESCRIPTOR_SETS_FALLBACK=ON` (required on macOS — MoltenVK
  does not implement descriptor buffers)

To **build** it you need, on every OS:

| Tool | Notes |
|---|---|
| CMake ≥ 3.22 + Ninja | |
| C++23 compiler | clang 17+/libc++, MSVC 19.38+, or Apple clang 17+ (`<expected>` is required) |
| [Vulkan SDK](https://vulkan.lunarg.com/) | provides headers, loader, validation layers, and **`slangc`** (must be on `PATH` — shaders are cooked at build time) |
| [vcpkg](https://github.com/microsoft/vcpkg) | manifest mode; all C++ libraries (SDL3, Jolt, EnTT, fastgltf, ImGui, glm, VMA, …) are downloaded and built automatically on first configure |

Set up vcpkg once (expected at `~/vcpkg`, or `%USERPROFILE%\vcpkg` on Windows):

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics      # bootstrap-vcpkg.bat on Windows
```

The first configure compiles all vcpkg dependencies (a few minutes); subsequent
configures hit the binary cache.

## Building

### Linux

Install the toolchain (Debian/Ubuntu example) and the Vulkan SDK, then use the
preset — it pins clang + libc++ and the vcpkg toolchain:

```sh
sudo apt install clang libc++-dev libc++abi-dev cmake ninja-build \
     libx11-dev libxft-dev libxext-dev libwayland-dev libxkbcommon-dev \
     libegl1-mesa-dev pkg-config           # X11/Wayland headers for SDL3
# install the Vulkan SDK (tarball or LunarG apt repo) and put slangc on PATH

cmake --preset release
cmake --build --preset release
./build/release/game
```

### Windows

Install Visual Studio 2022 (Desktop C++ workload), the Vulkan SDK, CMake, and
Ninja; vcpkg is expected at `%USERPROFILE%\vcpkg`. Then:

```bat
scripts\build-windows-release.bat
```

or use the `windows-release` CMake preset from a VS developer prompt.

### macOS (Apple Silicon, MoltenVK)

Install Xcode command-line tools, Homebrew `cmake` + `ninja`, and the macOS
Vulkan SDK (bundles MoltenVK). MoltenVK lacks `VK_EXT_descriptor_buffer`, so the
build must opt into the classic descriptor-set compatibility path — on platforms
with native descriptor-buffer support the fallback is never taken:

```sh
xcode-select --install
brew install cmake ninja

export VULKAN_SDK=$HOME/VulkanSDK/<version>/macOS
export PATH="$VULKAN_SDK/bin:$PATH"

cmake -S . -B build/macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENGINE_DESCRIPTOR_SETS_FALLBACK=ON \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/macos
./build/macos/game
```

### Running

`game` opens the editor (dock layout: hierarchy, inspector, viewport, asset
browser, animation preview, physics debug). `anim_smoke` in the same build
directory runs the windowless animation self-tests. Shaders are cooked into
`cooked/shaders/` automatically as part of the build; touch a `.slang` file and
rebuild to re-cook.

## Sample assets

Drop glTF models under `sample-assets/` (gitignored) and they appear in the editor's
asset browser; double-click or drag into the viewport to instantiate. The
[Khronos glTF sample assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
work well — e.g. `ToyCar`, `CarConcept`, `DamagedHelmet`, and the animated
`Fox` / `CesiumMan` / `BrainStem`:

```sh
git clone --filter=blob:none --sparse --depth 1 \
  https://github.com/KhronosGroup/glTF-Sample-Assets.git /tmp/gltf-samples
cd /tmp/gltf-samples && git sparse-checkout set Models/Fox Models/ToyCar
cp -R Models/Fox/glTF ~/quat-3d/sample-assets/Fox
```

## Repository layout

```
assets/shaders/   Slang sources (cooked to cooked/shaders/ at build time)
include/engine/   Public headers (core, rhi, renderer, animation, physics, scene, asset)
src/              Implementation, mirrored by module
src/editor/       ImGui editor layer (ENGINE_EDITOR builds only)
tests/            Windowless self-test harnesses (e.g. anim_smoke)
```

Development follows the phased plan in [`PHASES.md`](PHASES.md); engine conventions
live in [`CLAUDE.md`](CLAUDE.md).
