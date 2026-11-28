# quat-3d game engine — root build script.
#
# Toolchain: the project requires a C++23 standard library with <expected> and
# <flat_map>. On this machine only clang++ with libc++ provides both, so the
# presets pin CMAKE_CXX_COMPILER=clang++ and -stdlib=libc++. See CMakePresets.json.
#
# Note: CLAUDE.md targets CMake 3.28+, but the installed toolchain ships 3.22, so
# the minimum is pinned to 3.22. Raise this once a newer CMake is available.
cmake_minimum_required(VERSION 3.22)

# Overlay ports: local patches for upstream vcpkg ports (GameNetworkingSockets'
# libstdc++-only <debug/map> guard breaks the libc++ triplet). Must be set
# before project() so the vcpkg toolchain sees it.
set(VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/ports")

# Tracy profiling (R4): on by default for Debug / RelWithDebInfo (and an
# unset build type, which becomes Debug below), off for Release unless asked
# for (the `release` preset asks; packaged builds pass OFF). The built-in
# frame profiler is compiled either way. Decided before project() because
# Tracy is the vcpkg manifest feature `profiling`: only builds with
# ENGINE_PROFILE=ON download and build it.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    set(ENGINE_PROFILE_DEFAULT ON)
else()
    set(ENGINE_PROFILE_DEFAULT OFF)
endif()
option(ENGINE_PROFILE "Compile in the Tracy profiler client (on demand, loopback only)"
    ${ENGINE_PROFILE_DEFAULT})
if(ENGINE_PROFILE)
    list(APPEND VCPKG_MANIFEST_FEATURES "profiling")
endif()

project(quat_3d
    VERSION 0.1.0
    DESCRIPTION "Modular data-driven C++23 game engine"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ENGINE_EDITOR "Build the ImGui-based editor layer" ON)
# Compatibility switch for drivers without VK_EXT_descriptor_buffer (MoltenVK on
# macOS). When ON, the RHI may fall back to classic descriptor pools/sets and the
# instance opts into portability (non-conformant) ICD enumeration. Descriptor
# buffers remain the mandated model (CLAUDE.md); this exists only so the engine
# can run on platforms where the extension does not exist.
# Distribution build (release archives): the game embeds no build-machine
# paths and finds its data next to the executable (or via QUAT_ASSET_ROOT);
# see src/app/asset_root.hpp. Development builds also fall back to the tree.
option(ENGINE_PACKAGE_BUILD "Build for packaging: no source-tree data fallback" OFF)
option(ENGINE_DESCRIPTOR_SETS_FALLBACK
    "Allow classic descriptor-set fallback on drivers without VK_EXT_descriptor_buffer" OFF)

# ---------------------------------------------------------------------------
# Common compile options applied to all first-party engine targets.
# Third-party (vcpkg) targets are unaffected.
# ---------------------------------------------------------------------------
add_library(engine_flags INTERFACE)

target_compile_features(engine_flags INTERFACE cxx_std_23)

if(MSVC)
    # CMake's default MSVC flags enable exceptions and RTTI (/EHsc /GR); drop
    # them so engine_flags' /EHs-c- /GR- don't trip D9025 ("overriding")
    # on every translation unit. vcpkg ports are built separately.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    # Warning levels apply to the engine's own sources only (BUILD_INTERFACE):
    # SDK consumers (P8) get the language/ABI-relevant flags, not our warnings.
    target_compile_options(engine_flags INTERFACE
        $<BUILD_INTERFACE:/W4> /permissive- /EHs-c- /GR-)
    # getenv/fopen are used deliberately (portable C, bounds-checked call
    # sites); _HAS_EXCEPTIONS=0 matches /EHs-c- so the STL stops emitting
    # C4530 from its internal try/catch.
    target_compile_definitions(engine_flags INTERFACE
        _CRT_SECURE_NO_WARNINGS
        _HAS_EXCEPTIONS=0)
else()
    target_compile_options(engine_flags INTERFACE
        $<BUILD_INTERFACE:-Wall -Wextra -Wshadow>
        -fno-exceptions -fno-rtti)
endif()

# Third-party implementation TUs (VMA, stb, miniaudio, vendored backends) are
# compiled with warnings off: -w on clang/gcc. Nothing on MSVC — /w would
# collide with engine_flags' /W4 (D9025), and the third-party headers already
# arrive through SYSTEM include dirs, i.e. /external:I at /external:W0.
set(ENGINE_THIRD_PARTY_TU_FLAGS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")

# Per CLAUDE.md: column-major, reverse-Z friendly depth range [0, 1].
target_compile_definitions(engine_flags INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    $<$<CONFIG:Release>:NDEBUG>)

# ENGINE_EDITOR is defined per target (engine_editor_build below), not on
# engine_flags: an editor build also compiles quat-player (P4) from the same
# application sources without it.
add_library(engine_editor_build INTERFACE)
if(ENGINE_EDITOR)
    target_compile_definitions(engine_editor_build INTERFACE ENGINE_EDITOR)
endif()

if(ENGINE_DESCRIPTOR_SETS_FALLBACK)
    target_compile_definitions(engine_flags INTERFACE ENGINE_DESCRIPTOR_SETS_FALLBACK)
endif()

if(ENGINE_PACKAGE_BUILD)
    target_compile_definitions(engine_flags INTERFACE ENGINE_PACKAGE_BUILD)
endif()

# Apple's linker (ld-prime, Xcode 15+) warns about every static library CMake
# repeats on the link line — repetition CMake emits on purpose to resolve the
# engine libraries' mutual dependencies. Silence just that diagnostic where
# the linker understands the flag (older ld64 does not, and never warned).
if(APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries" ENGINE_LD_NO_WARN_DUPLICATE_LIBS)
    if(ENGINE_LD_NO_WARN_DUPLICATE_LIBS)
        target_link_options(engine_flags INTERFACE "LINKER:-no_warn_duplicate_libraries")
    endif()
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies.
#   - SDL3 from vcpkg (manifest mode; grown per phase).
#   - Vulkan from the system SDK/loader via CMake's bundled FindVulkan.
# ---------------------------------------------------------------------------
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)            # KTX2 + Basis Universal (R17)
find_package(meshoptimizer CONFIG REQUIRED)  # mesh codecs / optimisation (R17)
find_package(Stb REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(recastnavigation CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
# Lua 5.4 (C library, longjmp error handling) + sol2 (header-only bindings).
find_package(Lua REQUIRED)
# FindLua reports paths (LUA_LIBRARIES: liblua plus libm on Unix), not a
# target. Wrapped in an imported target so engine_script's export names it
# (quat3d_deps::lua) instead of the build machine's paths; quat3dConfig.cmake
# defines the same target from the consumer's own FindLua result (P8).
add_library(quat3d_deps::lua INTERFACE IMPORTED)
set_target_properties(quat3d_deps::lua PROPERTIES INTERFACE_LINK_LIBRARIES "${LUA_LIBRARIES}")
find_package(sol2 CONFIG REQUIRED)
# OpenSSL's libcrypto (GameNetworkingSockets already depends on it): the CSPRNG
# behind remote-control session tokens (engine_remote) and SHA-256 for game
# package manifests (engine_pack).
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchAcl.cmake) # ACL + RTM (header-only)
# miniaudio: vcpkg's port is header-only (it installs miniaudio.h and nothing
# else), so it is wrapped in an imported target here and engine_audio compiles
# the implementation (src/audio/miniaudio_impl.cpp). Linux needs pthread, libm
# and libdl (playback backends are dlopen'ed at runtime).
find_package(Threads REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
add_library(miniaudio_headers INTERFACE IMPORTED)
set_target_properties(miniaudio_headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MINIAUDIO_INCLUDE_DIR}")
target_link_libraries(miniaudio_headers INTERFACE
    Threads::Threads ${CMAKE_DL_LIBS} $<$<PLATFORM_ID:Linux,FreeBSD>:m>)
# Tracy client (manifest feature `profiling`, overlay port ports/tracy:
# TRACY_ON_DEMAND, loopback only). Its TRACY_ENABLE / TRACY_ON_DEMAND
# definitions travel with the target.
if(ENGINE_PROFILE)
    find_package(Tracy CONFIG REQUIRED)
endif()
if(ENGINE_EDITOR)
    find_package(imgui CONFIG REQUIRED)
    find_package(imguizmo CONFIG REQUIRED)
    find_package(unofficial-imgui-node-editor CONFIG REQUIRED)
endif()

# Public include directories. In the build tree: include/ plus the generated
# engine/version.hpp; installed (the SDK, P8): <prefix>/include, which holds
# only the stable header set (cmake/Quat3dInstall.cmake).
set(ENGINE_GENERATED_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
    ${ENGINE_GENERATED_INCLUDE}/engine/version.hpp @ONLY)
set(ENGINE_PUBLIC_INCLUDE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${ENGINE_GENERATED_INCLUDE}>
    $<INSTALL_INTERFACE:include>)

# ---------------------------------------------------------------------------
# Engine modules: one CMakeLists.txt per src/<module> (ROADMAP S9), each
# defining its own libraries, sources and links. The order is the build's
# definition order (the SDK's derived library list follows it).
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/project)
add_subdirectory(src/input)
add_subdirectory(src/rhi)
add_subdirectory(src/asset)
add_subdirectory(src/animation)
add_subdirectory(src/astro)
add_subdirectory(src/physics)
add_subdirectory(src/scene)
add_subdirectory(src/script)
add_subdirectory(src/cook)
add_subdirectory(src/terrain)
add_subdirectory(src/weather)
add_subdirectory(src/nav)
add_subdirectory(src/net)
add_subdirectory(src/audio)
add_subdirectory(src/remote)
add_subdirectory(src/renderer)
add_subdirectory(src/editor)
add_subdirectory(src/app)
add_subdirectory(src/pack)

# ---------------------------------------------------------------------------
# Offline asset generator (procedural nature models -> assets/meshes/nature/).
# ---------------------------------------------------------------------------
add_subdirectory(tools/asset-gen)

# ---------------------------------------------------------------------------
# Tests (GoogleTest) — see tests/CMakeLists.txt. `ctest --test-dir build/<preset>`.
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Offline shader cooking: assets/shaders/*.slang -> cooked/shaders/*.{spv,json}
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)
add_shader_compilation(shaders)
add_dependencies(game shaders)
add_dependencies(quat-player shaders)
add_dependencies(quat-player-shipping shaders)
# Shader hot reload (R2) runs the same slangc the build cooks with. Not in
# packaged builds: no build-machine paths there, and hot reload defaults off.
if(NOT ENGINE_PACKAGE_BUILD)
    target_compile_definitions(engine_rhi PRIVATE QUAT_SLANGC_PATH="${SLANGC_EXECUTABLE}")
endif()

# ---------------------------------------------------------------------------
# Engine SDK (ROADMAP P8): umbrella targets, install rules, the quat3d CMake
# package, the toolchain file and the SDK tools. See docs/sdk.md.
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Quat3dInstall.cmake)
