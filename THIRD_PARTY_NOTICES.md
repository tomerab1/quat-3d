# Third-party notices

quat-3d is licensed under the MIT License (see `LICENSE`). It is built on the
third-party software listed below, each under its own licence.

In the release archives, this file is followed by the full licence text of
every component. The release packaging (`scripts/third-party-notices.sh`)
appends the texts from the build: vcpkg installs each port's licence as
`share/<port>/copyright`, and FetchContent leaves each source tree in place.
In the repository, this file is the summary, and
`scripts/check-third-party-notices.py` (run in CI) checks that every vcpkg
dependency, FetchContent package and bundled font has a row here.

Licences are SPDX identifiers. The ones for vcpkg ports come from the port
metadata at the manifest's `builtin-baseline`.

## Libraries (vcpkg, `vcpkg.json`)

| ID | Component | Licence | Upstream | In the archives |
|---|---|---|---|---|
| `sdl3` | SDL 3 | Zlib AND MIT AND Apache-2.0 | https://www.libsdl.org | linked |
| `vulkan-memory-allocator` | Vulkan Memory Allocator | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | linked |
| `nlohmann-json` | JSON for Modern C++ | MIT | https://github.com/nlohmann/json | linked |
| `glm` | OpenGL Mathematics | MIT | https://github.com/g-truc/glm | linked |
| `fastgltf` | fastgltf | MIT | https://github.com/spnda/fastgltf | linked |
| `ktx` | KTX-Software (libktx, with the Basis Universal transcoder) | Apache-2.0, with component licences in its `LICENSES/` | https://github.com/KhronosGroup/KTX-Software | linked (the overlay port builds it without the non-open-source ETC decoder) |
| `meshoptimizer` | meshoptimizer | MIT | https://github.com/zeux/meshoptimizer | linked |
| `stb` | stb single-file libraries | MIT OR CC-PDDC | https://github.com/nothings/stb | linked |
| `entt` | EnTT | MIT | https://github.com/skypjack/entt | linked |
| `joltphysics` | Jolt Physics | MIT | https://github.com/jrouwe/JoltPhysics | linked |
| `imgui` | Dear ImGui (docking), plus its SDL3 backend vendored in `src/editor/vendor/` | MIT | https://github.com/ocornut/imgui | linked |
| `imguizmo` | ImGuizmo | MIT | https://github.com/CedricGuillemet/ImGuizmo | linked |
| `imgui-node-editor` | imgui-node-editor | MIT | https://github.com/thedmd/imgui-node-editor | linked |
| `recastnavigation` | Recast & Detour | Zlib | https://github.com/recastnavigation/recastnavigation | linked |
| `gamenetworkingsockets` | GameNetworkingSockets | BSD-3-Clause | https://github.com/ValveSoftware/GameNetworkingSockets | linked |
| `openssl` | OpenSSL | Apache-2.0 | https://www.openssl.org | linked |
| `miniaudio` | miniaudio | Unlicense OR MIT-0 | https://github.com/mackron/miniaudio | linked |
| `lua` | Lua 5.4 | MIT | https://www.lua.org | linked |
| `sol2` | sol2 | MIT | https://github.com/ThePhD/sol2 | linked |
| `gtest` | GoogleTest | BSD-3-Clause | https://github.com/google/googletest | no: tests only |

These ports pull in further libraries, whose licence texts are also appended
in the archives. They include `protobuf` (BSD-3-Clause), `abseil`
(Apache-2.0) and `utf8-range` (MIT) for GameNetworkingSockets, and `zstd`
(BSD-3-Clause OR GPL-2.0-only, used under BSD-3-Clause) for KTX-Software.

## Libraries fetched at configure time (CMake FetchContent)

| ID | Component | Licence | Upstream | In the archives |
|---|---|---|---|---|
| `acl` | Animation Compression Library v2.1.0 | MIT | https://github.com/nfrechette/acl | linked (header-only) |
| `rtm` | Realtime Math v2.2.0 | MIT | https://github.com/nfrechette/rtm | linked (header-only) |

## Code ported into quat-3d

| ID | Component | Licence | Upstream | In the archives |
|---|---|---|---|---|
| `xegtao` | XeGTAO: `assets/shaders/gtao.slang` and `gtao_denoise.slang` are Slang ports of it | MIT | https://github.com/GameTechDev/XeGTAO | shader sources and compiled SPIR-V |

## Fonts and icons (`assets/fonts/`)

| ID | Component | Licence | Upstream | In the archives |
|---|---|---|---|---|
| `inter` | Inter | OFL-1.1 | https://github.com/rsms/inter | `assets/fonts/inter/` |
| `jetbrains-mono` | JetBrains Mono | OFL-1.1 | https://github.com/JetBrains/JetBrainsMono | `assets/fonts/jetbrains-mono/` |
| `lucide` | Lucide icons | ISC | https://lucide.dev | `assets/fonts/lucide/` |

## Platform runtimes shipped with the game

| ID | Component | Licence | Upstream | In the archives |
|---|---|---|---|---|
| `libcxx` | LLVM libc++ / libc++abi / libunwind | Apache-2.0 WITH LLVM-exception | https://libcxx.llvm.org | Linux: `lib/` |
| `msvc-runtime` | Microsoft Visual C++ runtime DLLs | Microsoft Visual Studio redistributable terms | https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files | Windows: next to `game.exe` |
| `moltenvk` | MoltenVK | Apache-2.0 | https://github.com/KhronosGroup/MoltenVK | macOS: `Quat3D.app/Contents/Frameworks/` |
| `vulkan-loader` | Khronos Vulkan loader | Apache-2.0 | https://github.com/KhronosGroup/Vulkan-Loader | macOS: `Quat3D.app/Contents/Frameworks/` |

On Linux and Windows, the Vulkan loader and driver come from the system.

## Not in the archives

- **`quat-mcp`** (`tools/quat-mcp`) is built from source with Cargo. Its
  crates carry their own licences; `cargo tree` lists them.
- **Poly Haven models** that `asset_fetch` downloads are CC0. Each is saved
  with a `LICENSE.md` in the gitignored `assets/meshes/polyhaven/<id>/`.
