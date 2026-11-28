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
# engine_core — graphics-free foundations: the log ring + log() helper, stderr
# capture into the ring, the simulation clock (A5; S6 grows the logging layer
# here), file watching and child processes (shader hot reload, R2), and the
# runtime data-root resolution used by the game and packaged builds (P4).
# ---------------------------------------------------------------------------
add_library(engine_core STATIC
    src/core/log.cpp
    src/core/stderr_capture.cpp
    src/core/sim_clock.cpp
    src/core/event_ring.cpp
    src/core/frame_timer.cpp
    src/core/path_confine.cpp
    src/core/file_watcher.cpp
    src/core/child_process.cpp
    src/core/asset_root.cpp
    src/core/path_confine.cpp
    src/core/env.cpp
    src/core/profile_names.cpp
    src/core/profile_stats.cpp
    src/core/profile_capture.cpp
    src/core/frame_profiler.cpp
    src/core/version.cpp)
# glm + nlohmann_json are PUBLIC: engine/core/json_fields.hpp (header-only,
# part of the SDK's stable API) reads JSON into glm types.
target_link_libraries(engine_core PUBLIC engine_flags Threads::Threads
    glm::glm nlohmann_json::nlohmann_json)
target_include_directories(engine_core PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# QUAT_TRACY switches engine/core/profile.hpp's macros (and the GPU zones of
# rhi::GpuProfiler) to Tracy as well; PUBLIC so every engine target that
# instruments code sees the same configuration.
if(ENGINE_PROFILE)
    target_link_libraries(engine_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(engine_core PUBLIC QUAT_TRACY)
endif()

# ---------------------------------------------------------------------------
# engine_project — game projects (ROADMAP P4): the game.json descriptor and
# its validation, the package manifest, and BuildJob (quat-pack run in the
# background for the editor's export dialog and remote project.build).
# Graphics-free.
# ---------------------------------------------------------------------------
add_library(engine_project STATIC
    src/project/game_project.cpp
    src/project/package_manifest.cpp
    src/project/build_job.cpp)
target_link_libraries(engine_project PUBLIC engine_flags engine_core nlohmann_json::nlohmann_json)
target_link_libraries(engine_project PRIVATE glm::glm Threads::Threads)
target_include_directories(engine_project PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_input — action maps (G5): device state, JSON action maps, the
# Actions evaluator (contexts, triggers, modifiers, injection, rebinding) and
# the per-user rebinding file. Window-free: the app samples SDL3 into a
# DeviceState (src/app/input_sampler.cpp).
# ---------------------------------------------------------------------------
add_library(engine_input STATIC
    src/input/device_names.cpp
    src/input/binding_json.cpp
    src/input/action_map_json.cpp
    src/input/default_map.cpp
    src/input/source_read.cpp
    src/input/actions.cpp
    src/input/overrides.cpp)
target_link_libraries(engine_input PUBLIC engine_flags engine_core nlohmann_json::nlohmann_json)
target_include_directories(engine_input PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_rhi — thin Vulkan abstraction (instance/device, swapchain, ...).
# ---------------------------------------------------------------------------
add_library(engine_rhi STATIC
    src/rhi/device.cpp
    src/rhi/queue_selection.cpp
    src/rhi/swapchain.cpp
    src/rhi/gpu_allocator.cpp
    src/rhi/image_upload.cpp
    src/rhi/vma_impl.cpp
    src/rhi/shader_reflection.cpp
    src/rhi/pipeline_cache.cpp
    src/rhi/descriptor_buffer.cpp
    src/rhi/render_graph.cpp
    src/rhi/gpu_profiler.cpp
    src/rhi/graphics_pipeline.cpp
    src/rhi/compute_pipeline.cpp
    src/rhi/shader_compiler.cpp
    src/rhi/shader_diagnostics.cpp
    src/rhi/shader_hot_reload.cpp
    src/rhi/material_shader_registry.cpp
    src/rhi/material_shader_source.cpp)
target_link_libraries(engine_rhi PUBLIC
    engine_flags engine_core Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
# engine_core is public: shader hot reload's headers use its watcher/process
# types, and the debug messenger routes validation messages into its log ring.
# The hot-reload compile worker runs on a std::thread; hot reload defaults on
# in editor builds (engine_editor_build).
target_link_libraries(engine_rhi PRIVATE Threads::Threads engine_editor_build)
# nlohmann_json is an implementation detail of the reflection parser (PRIVATE);
# glm because the parser reads slangc's JSON through core::JsonFields.
target_link_libraries(engine_rhi PRIVATE nlohmann_json::nlohmann_json glm::glm)
target_include_directories(engine_rhi PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# VMA's implementation is not warning-clean under our strict flags; compile just
# that TU permissively (our code using VMA stays strict).
set_source_files_properties(src/rhi/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")

# ---------------------------------------------------------------------------
# engine_asset — asset identity, ref-counted handles, synchronous load path,
# and asset types (MeshAsset). MeshAsset owns GpuBuffers, so engine_rhi and glm
# are PUBLIC: any consumer of the asset headers needs them transitively.
# ---------------------------------------------------------------------------
# engine_image — graphics-free texture data, limits and the guarded PNG/JPEG
# and Radiance HDR decoders (stb_image), shared by engine_asset and the cooker.
add_library(engine_image STATIC
    src/asset/alpha_coverage.cpp
    src/asset/texture_data.cpp
    src/asset/image_decode.cpp
    src/asset/cube_lut.cpp
    src/asset/hdr_decode.cpp)
target_link_libraries(engine_image PUBLIC engine_flags)
target_link_libraries(engine_image PRIVATE engine_stb_image)
target_include_directories(engine_image PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_image PRIVATE ${Stb_INCLUDE_DIR})

# libktx (KTX2 parsing, Zstd, Basis Universal transcoding) backs the KTX2
# loader. A program can hold only one libktx archive (ktx and ktx_read share
# objects: duplicate symbols), so engine_asset's link interface picks it for
# the executable being linked: KTX::ktx_read (decoder only; ports/ktx,
# game_has_no_texture_encoder) unless the executable sets QUAT_KTX_FULL, as
# the cooker and the tests do (they need the full KTX::ktx for encoding).
# The bare $<TARGET_PROPERTY:QUAT_KTX_FULL> is evaluated on the consuming
# executable, and as engine_asset's own dependency the archive is emitted
# after every copy of libengine_asset.a on the link line.
add_library(engine_asset STATIC
    src/asset/asset_manager.cpp
    src/asset/material_asset.cpp
    src/asset/ktx2_validate.cpp
    src/asset/ktx2_transcode.cpp
    src/asset/texture_upload.cpp)
target_link_libraries(engine_asset PUBLIC engine_flags engine_rhi engine_image glm::glm)
target_link_libraries(engine_asset PRIVATE
    $<IF:$<BOOL:$<TARGET_PROPERTY:QUAT_KTX_FULL>>,KTX::ktx,KTX::ktx_read>)
target_include_directories(engine_asset PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_animation — CPU-side skeletal animation (skeletons, clips, animator,
# inertialization, two-bone / foot IK, ACL clip compression). Graphics-free:
# plain data + math, consumed by engine_scene and the GPU skinning pass.
# ---------------------------------------------------------------------------
add_library(engine_animation STATIC
    src/animation/skeleton.cpp
    src/animation/clip.cpp
    src/animation/animator.cpp
    src/animation/state_machine.cpp
    src/animation/inertialization.cpp
    src/animation/clip_resample.cpp
    src/animation/acl_compress.cpp
    src/animation/acl_decompress.cpp
    src/animation/ik/two_bone_ik.cpp
    src/animation/ik/foot_placement.cpp)
target_link_libraries(engine_animation PUBLIC engine_flags glm::glm)
# ACL is an implementation detail of the clip codec (acl_*.cpp).
# BUILD_INTERFACE: header-only and fetched at configure time, so the SDK's
# exported engine_animation (a static library) needs nothing from it.
target_link_libraries(engine_animation PRIVATE $<BUILD_INTERFACE:acl_headers>)
target_include_directories(engine_animation PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_astro — astronomy for the time of day (R22.1): calendar / Julian
# dates, sidereal time, precession, the Meeus Sun and Moon, rise / set,
# and the renderer-facing SkyState. Pure, deterministic double-precision CPU
# code; glm only.
# ---------------------------------------------------------------------------
add_library(engine_astro STATIC
    src/astro/time.cpp
    src/astro/coords.cpp
    src/astro/sun.cpp
    src/astro/moon.cpp
    src/astro/moon_terms.cpp
    src/astro/ephemeris.cpp
    src/astro/events.cpp
    src/astro/sky_state.cpp)
target_link_libraries(engine_astro PUBLIC engine_flags glm::glm)
target_include_directories(engine_astro PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_physics — Jolt Physics wrapper (PhysicsWorld). Jolt is an
# implementation detail (PRIVATE); the public header is Jolt-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_physics STATIC
    src/physics/physics_world.cpp
    src/physics/physics_character.cpp
    src/physics/physics_queries.cpp
    src/physics/physics_events.cpp
    src/physics/physics_body_control.cpp
    src/physics/physics_shape_queries.cpp
    src/physics/physics_joints.cpp)
# glm is PUBLIC: physics_world.hpp's API takes glm vectors/quaternions.
target_link_libraries(engine_physics PUBLIC engine_flags glm::glm)
target_link_libraries(engine_physics PRIVATE engine_core) # profiler zones + worker names (R4)
target_link_libraries(engine_physics PRIVATE Jolt::Jolt)
target_include_directories(engine_physics PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_scene — scene-construction side of the asset pipeline. For now just the
# glTF loader (fastgltf -> MeshAsset); gains the ECS world + systems in Phase 4.
# fastgltf is an implementation detail of the loader (PRIVATE).
# ---------------------------------------------------------------------------
add_library(engine_scene STATIC
    src/scene/gltf/mesh.cpp
    src/scene/gltf/material.cpp
    src/scene/gltf/skin_animation.cpp
    src/scene/gltf/clip_compression.cpp
    src/scene/gltf/scene_assembly.cpp
    src/scene/gltf/self_tests.cpp
    src/scene/gltf/texture.cpp
    src/scene/components.cpp
    src/scene/scene.cpp
    src/scene/animation_system.cpp
    src/scene/physics_system.cpp
    src/scene/joint_system.cpp
    src/scene/nav_agent_system.cpp
    src/scene/nav_separation.cpp
    src/scene/navigation.cpp
    src/scene/behavior.cpp
    src/scene/bt_graph.cpp
    src/scene/bt_graph_edit.cpp
    src/scene/scene_io.cpp
    src/scene/scene_references.cpp
    src/scene/component_json.cpp
    src/scene/component_json_physics.cpp
    src/scene/lens_json.cpp
    src/scene/particle_json.cpp
    src/scene/script_component.cpp
    src/scene/scene_snapshot.cpp
    src/scene/component_json_animation.cpp
    src/scene/material_override.cpp
    src/scene/render_settings_json.cpp
    src/scene/physics_settings.cpp
    src/scene/weather_json.cpp
    src/scene/weather_frame.cpp
    src/scene/audio_system.cpp
    src/scene/primitives.cpp
    src/scene/primitive_resolve.cpp
    src/scene/scatter.cpp
    src/scene/instanced_mesh.cpp
    src/scene/instanced_mesh_resolve.cpp
    src/scene/ground_placement.cpp
    src/scene/camera_framing.cpp
    src/scene/camera_rig.cpp
    src/scene/camera_rig_system.cpp
    src/scene/component_json_camera_rig.cpp
    src/scene/world_clock.cpp
    src/scene/world_clock_json.cpp
    src/scene/world_clock_api.cpp
    src/scene/season.cpp
    src/scene/season_json.cpp
    src/scene/season_api.cpp
    src/scene/bounds.cpp
    src/scene/view_clip.cpp)
# EnTT is PUBLIC: components.hpp exposes entt types (Parent/Children) to consumers.
# engine_renderer is PUBLIC: scene.hpp's RenderCollectSystem fills a renderer
# DrawItem list (no cycle — the renderer does not depend on the scene).
# engine_animation is PUBLIC: components.hpp's SkinnedMesh holds a SkeletonAsset
# handle and the glTF loader extracts skins.
target_link_libraries(engine_scene PUBLIC
    engine_flags engine_input engine_asset engine_rhi engine_renderer engine_animation engine_physics
    engine_terrain engine_nav engine_audio engine_weather engine_astro EnTT::EnTT)
# fastgltf and stb (image decode) are implementation details of the loader.
# nlohmann_json is PUBLIC: component_json.hpp exposes JSON values (the codec
# shared by scene files and the remote-control API).
target_link_libraries(engine_scene PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(engine_scene PRIVATE fastgltf::fastgltf engine_gltf_io)
target_include_directories(engine_scene SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(engine_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(engine_scene PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# engine_gltf_io — glTF parse + validation, EXT_meshopt_compression decoding
# and buffer access (src/scene/gltf/gltf_io.hpp): fastgltf only, no ECS or
# GPU, so the cooker parses and validates exactly like the runtime loader.
add_library(engine_gltf_io STATIC
    src/scene/gltf/validate.cpp
    src/scene/gltf/meshopt_decode.cpp
    src/scene/gltf/gltf_source.cpp)
target_link_libraries(engine_gltf_io PUBLIC engine_flags engine_image fastgltf::fastgltf)
target_link_libraries(engine_gltf_io PRIVATE meshoptimizer::meshoptimizer)
target_include_directories(engine_gltf_io PUBLIC
    ${ENGINE_PUBLIC_INCLUDE} $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

# stb_image's implementation (PNG + JPEG only), used through engine_image's
# guarded decoder. Its TU is not warning-clean under our strict flags.
add_library(engine_stb_image STATIC src/scene/stb_image_impl.cpp)
target_link_libraries(engine_stb_image PRIVATE engine_flags)
target_include_directories(engine_stb_image SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
set_source_files_properties(src/scene/stb_image_impl.cpp PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")

# ---------------------------------------------------------------------------
# engine_script — Lua gameplay scripting (G7): one sandboxed Lua 5.4 state
# per play session with per-call instruction / memory limits, the Script
# component's runtime, hot reload, and the engine API scripts call (entities,
# components through the codec, physics, input, audio, navigation). Lua and
# sol2 are implementation details (PRIVATE); the public headers are Lua-free.
# sol2 runs without exceptions (the engine builds with -fno-exceptions) and
# with every safety check on.
# ---------------------------------------------------------------------------
add_library(engine_script STATIC
    src/script/lua_vm.cpp
    src/script/lua_bind.cpp
    src/script/lua_json.cpp
    src/script/script_api_math.cpp
    src/script/script_api_entity.cpp
    src/script/script_api_scene.cpp
    src/script/script_api_services.cpp
    src/script/script_api_input.cpp
    src/script/script_source.cpp
    src/script/script_runtime.cpp
    src/script/script_files.cpp)
target_link_libraries(engine_script PUBLIC engine_flags engine_core engine_input engine_scene engine_physics)
target_link_libraries(engine_script PRIVATE sol2::sol2 quat3d_deps::lua)
# SYSTEM: the vcpkg include directory holds every dependency's headers.
target_include_directories(engine_script SYSTEM PRIVATE ${LUA_INCLUDE_DIR})
target_include_directories(engine_script PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_compile_definitions(engine_script PRIVATE SOL_NO_EXCEPTIONS=1 SOL_ALL_SAFETIES_ON=1)

# ---------------------------------------------------------------------------
# engine_cook — offline asset cooker (R17): glTF textures -> KTX2/Basis,
# meshes -> meshoptimizer passes, quantisation and EXT_meshopt_compression.
# Used by tools/quat-cook and the tests; never linked into the game.
# ---------------------------------------------------------------------------
add_library(engine_cook STATIC
    src/cook/gltf_access.cpp
    src/cook/gltf_cooker.cpp
    src/cook/mesh_cook.cpp
    src/cook/mesh_simplify.cpp
    src/cook/mesh_streams.cpp
    src/cook/texture_encode.cpp)
target_link_libraries(engine_cook PUBLIC engine_flags)
target_link_libraries(engine_cook PRIVATE
    engine_image engine_gltf_io fastgltf::fastgltf KTX::ktx meshoptimizer::meshoptimizer)
target_include_directories(engine_cook PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_cook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(quat-cook tools/quat-cook/main.cpp)
target_link_libraries(quat-cook PRIVATE engine_flags engine_cook)
# Full libktx (engine_cook encodes); see engine_asset.
set_target_properties(quat-cook PROPERTIES QUAT_KTX_FULL ON)

# ---------------------------------------------------------------------------
# engine_terrain — procedural terrain (Phase 12). Pure CPU heightmap generation
# in 12.1; chunked LOD rendering and Jolt height fields layer on in 12.2/12.3.
# ---------------------------------------------------------------------------
add_library(engine_terrain STATIC
    src/terrain/budget.cpp
    src/terrain/cpu_tile_jobs.cpp
    src/terrain/generator.cpp
    src/terrain/grid_erosion.cpp
    src/terrain/streamer.cpp
    src/terrain/talus_relax.cpp)
target_link_libraries(engine_terrain PUBLIC engine_flags glm::glm)
target_link_libraries(engine_terrain PRIVATE engine_core) # profiler zones + thread names (R4)
target_include_directories(engine_terrain PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_weather — the scene's weather (R13.1): the WeatherState singleton
# component, its field table and accumulation, and the CPU reference of the
# global wind field the renderer evaluates on the GPU. Graphics-free.
# ---------------------------------------------------------------------------
add_library(engine_weather STATIC
    src/weather/weather_state.cpp
    src/weather/climate.cpp
    src/weather/climate_sequence.cpp
    src/weather/wind_field.cpp)
target_link_libraries(engine_weather PUBLIC engine_flags glm::glm EnTT::EnTT)
target_include_directories(engine_weather PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_nav — Recast/Detour navigation (Phase 13). Recast is an implementation
# detail (PRIVATE); the public header is Recast-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_nav STATIC
    src/nav/navmesh.cpp)
target_link_libraries(engine_nav PUBLIC engine_flags glm::glm)
target_link_libraries(engine_nav PRIVATE
    RecastNavigation::Recast RecastNavigation::Detour RecastNavigation::DetourCrowd)
target_include_directories(engine_nav PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_net — GameNetworkingSockets transport (Phase 14). GNS is an
# implementation detail (PRIVATE); the public header is GNS-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_net STATIC
    src/net/character_step.cpp
    src/net/net_ids.cpp
    src/net/transport.cpp
    src/net/transport_common.cpp
    src/net/host_access.cpp
    src/net/session_config.cpp
    src/net/replication.cpp
    src/net/replication_transport.cpp
    src/net/prediction.cpp
    src/net/session.cpp)
target_link_libraries(engine_net PUBLIC engine_flags glm::glm EnTT::EnTT)
# Linux (static triplet) exports ::static; Windows (dynamic) exports ::shared.
if(TARGET GameNetworkingSockets::static)
    target_link_libraries(engine_net PRIVATE GameNetworkingSockets::static)
else()
    target_link_libraries(engine_net PRIVATE GameNetworkingSockets::shared)
endif()
target_link_libraries(engine_net PRIVATE engine_scene)
target_include_directories(engine_net PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_audio — miniaudio mixer, buses and 3D sound (G1). miniaudio is an
# implementation detail (PRIVATE); the public header is miniaudio-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_audio STATIC
    src/audio/audio_engine.cpp
    src/audio/miniaudio_impl.cpp)
target_link_libraries(engine_audio PUBLIC engine_flags glm::glm)
# miniaudio_headers is a local imported target holding a build-machine path:
# BUILD_INTERFACE keeps it out of the SDK export; its link libraries (threads,
# libdl, libm) are repeated so the exported static library carries them.
target_link_libraries(engine_audio PRIVATE $<BUILD_INTERFACE:miniaudio_headers> engine_core
    Threads::Threads ${CMAKE_DL_LIBS} $<$<PLATFORM_ID:Linux,FreeBSD>:m>)
target_include_directories(engine_audio PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# miniaudio's implementation is not warning-clean under our strict flags.
set_source_files_properties(src/audio/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")

# ---------------------------------------------------------------------------
# engine_remote — remote-control endpoint (Phase 15): loopback JSON-RPC 2.0
# over TCP, poll-driven on the main thread. Out-of-process tools (the quat-mcp
# Model Context Protocol bridge in tools/quat-mcp) drive the engine through it.
# nlohmann_json is PUBLIC: the handler API traffics in JSON values. OpenSSL's
# libcrypto (PRIVATE) generates the session tokens (session_token.cpp).
# ---------------------------------------------------------------------------
add_library(engine_remote STATIC
    src/remote/rpc_server.cpp
    src/remote/session_token.cpp
    src/remote/socket.cpp)
target_link_libraries(engine_remote PUBLIC engine_flags nlohmann_json::nlohmann_json)
target_link_libraries(engine_remote PRIVATE OpenSSL::Crypto)
target_include_directories(engine_remote PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# The user-only token file: one implementation per platform (private_file.hpp).
if(WIN32)
    target_sources(engine_remote PRIVATE src/remote/private_file_win32.cpp)
    target_link_libraries(engine_remote PRIVATE ws2_32 advapi32)
else()
    target_sources(engine_remote PRIVATE src/remote/private_file_posix.cpp)
endif()

# ---------------------------------------------------------------------------
# engine_remote_world — the world commands (ROADMAP A2/A3): scene.*,
# environment.*, camera.*, render.screenshot, asset.*, sim.*, nav.bake, debug.*
# and bt.* (behaviour-tree authoring), on top of engine_remote + the scene
# codec and the behaviour-tree graph model. Application-only capabilities come in
# through WorldServices callbacks (src/app/remote_bridge.cpp wires them), so
# the commands are testable on a bare Scene.
# ---------------------------------------------------------------------------
add_library(engine_remote_world STATIC
    src/remote/world/common.cpp
    src/remote/world/scene_commands.cpp
    src/remote/world/batch_command.cpp
    src/remote/world/environment_commands.cpp
    src/remote/world/terrain_commands.cpp
    src/remote/world/surface_query.cpp
    src/remote/world/placement_commands.cpp
    src/remote/world/camera_commands.cpp
    src/remote/world/app_commands.cpp
    src/remote/world/debug_commands.cpp
    src/remote/world/debug_query_commands.cpp
    src/remote/world/shader_commands.cpp
    src/remote/world/tuning_commands.cpp
    src/remote/world/behavior_commands.cpp
    src/remote/world/behavior_edit_commands.cpp
    src/remote/world/physics_commands.cpp
    src/remote/world/physics_body_commands.cpp
    src/remote/world/physics_query_commands.cpp
    src/remote/world/weather_commands.cpp
    src/remote/world/project_commands.cpp
    src/remote/world/time_commands.cpp
    src/remote/world/script_commands.cpp
    src/remote/world/input_commands.cpp
    src/remote/world/camera_rig_commands.cpp
    src/remote/world/profile_commands.cpp
    src/remote/world/image_encode.cpp
    src/remote/world/stb_image_write_impl.cpp)
target_link_libraries(engine_remote_world PUBLIC
    engine_flags engine_core engine_input engine_remote engine_scene engine_physics engine_nav engine_weather
    engine_script engine_project)
target_include_directories(engine_remote_world PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_remote_world SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# stb's implementation TU is not warning-clean under our strict flags.
set_source_files_properties(src/remote/world/stb_image_write_impl.cpp
    PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")

# ---------------------------------------------------------------------------
# engine_renderer — high-level passes built on the RHI. Starts with the deferred
# GBuffer pass (mesh_pass); lighting/tonemap/imgui passes follow in later slices.
# ---------------------------------------------------------------------------
add_library(engine_renderer STATIC
    src/renderer/csm.cpp
    src/renderer/draw_batching.cpp
    src/renderer/render_settings.cpp
    src/renderer/grass.cpp
    src/renderer/grass_pass.cpp
    src/renderer/gtao_pass.cpp
    src/renderer/hiz_pass.cpp
    src/renderer/instance_buffer.cpp
    src/renderer/non_finite_counters.cpp
    src/renderer/mesh_pass.cpp
    src/renderer/material_pipelines.cpp
    src/renderer/lighting_pass.cpp
    src/renderer/light_clusters.cpp
    src/renderer/light_cull_pass.cpp
    src/renderer/particles.cpp
    src/renderer/particle_emitters.cpp
    src/renderer/particle_pass.cpp
    src/renderer/particle_sort.cpp
    src/renderer/particle_draw.cpp
    src/renderer/tonemap_pass.cpp
    src/renderer/color_grading.cpp
    src/renderer/color_lut.cpp
    src/renderer/physical_camera.cpp
    src/renderer/post_stage.cpp
    src/renderer/dof_pass.cpp
    src/renderer/motion_blur_pass.cpp
    src/renderer/skinning_pass.cpp
    src/renderer/shadow_casters.cpp
    src/renderer/shadow_pass.cpp
    src/renderer/transparent_pass.cpp
    src/renderer/ibl_pass.cpp
    src/renderer/dynamic_ibl.cpp
    src/renderer/compute_kernel.cpp
    src/renderer/hdri_environment.cpp
    src/renderer/hdri_sun.cpp
    src/renderer/ssr_pass.cpp
    src/renderer/bloom_pass.cpp
    src/renderer/exposure_pass.cpp
    src/renderer/taa_pass.cpp
    src/renderer/jitter.cpp
    src/renderer/velocity.cpp
    src/renderer/velocity_pass.cpp
    src/renderer/motion_history.cpp
    src/renderer/upscaler.cpp
    src/renderer/atmosphere_pass.cpp
    src/renderer/terrain_pass.cpp
    src/renderer/terrain_gpu_tiles.cpp
    src/renderer/terrain_synth_pass.cpp
    src/renderer/terrain_shadow.cpp
    src/renderer/wind_field_pass.cpp
    src/renderer/cloud_shadow_pass.cpp
    src/renderer/cloud_shadow_reference.cpp)
target_link_libraries(engine_renderer PUBLIC
    engine_flags engine_rhi engine_asset engine_terrain engine_weather glm::glm)
target_include_directories(engine_renderer PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# The ImGui overlay pass is editor-only. It is a from-scratch renderer on the
# engine RHI (descriptor buffers — the stock imgui_impl_vulkan backend uses
# descriptor pools, which may not be mixed with descriptor buffers in one
# command buffer). imgui_pass.cpp is the only renderer TU allowed to include
# ImGui headers.
if(ENGINE_EDITOR)
    target_sources(engine_renderer PRIVATE src/renderer/imgui_pass.cpp)
    target_link_libraries(engine_renderer PRIVATE imgui::imgui)
endif()

# ---------------------------------------------------------------------------
# engine_editor — ImGui editor layer (context, SDL3 platform backend, panels).
# The SDL3 input backend is vendored (src/editor/vendor) rather than pulled via
# imgui[sdl3-binding]: that vcpkg feature re-enables SDL3's default features
# (dbus/ibus -> libsystemd -> libxcrypt), a dependency chain the engine does
# not want.
# ---------------------------------------------------------------------------
if(ENGINE_EDITOR)
    add_library(engine_editor STATIC
        src/editor/editor.cpp
        src/editor/scene_hierarchy.cpp
        src/editor/inspector.cpp
        src/editor/tuning_panels.cpp
        src/editor/weather_panel.cpp
        src/editor/time_of_day_panel.cpp
        src/editor/asset_browser.cpp
        src/editor/animation_preview.cpp
        src/editor/physics_debug.cpp
        src/editor/picking.cpp
        src/editor/audio_inspector.cpp
        src/editor/script_inspector.cpp
        src/editor/animation_inspector.cpp
        src/editor/camera_rig_inspector.cpp
        src/editor/grass_inspector.cpp
        src/editor/particle_inspector.cpp
        src/editor/physics_inspector.cpp
        src/editor/behavior_panel.cpp
        src/editor/behavior_canvas.cpp
        src/editor/inspector_world.cpp
        src/editor/panels/console.cpp
        src/editor/panels/inspector_parts.cpp
        src/editor/panels/profiler.cpp
        src/editor/shell/command_palette.cpp
        src/editor/shell/commands.cpp
        src/editor/shell/dialogs.cpp
        src/editor/shell/editor_shell.cpp
        src/editor/shell/export_dialog.cpp
        src/editor/shell/fuzzy.cpp
        src/editor/shell/layout.cpp
        src/editor/shell/menu.cpp
        src/editor/shell/panels.cpp
        src/editor/shell/prefs.cpp
        src/editor/shell/scene_actions.cpp
        src/editor/shell/stats_panel.cpp
        src/editor/shell/status_bar.cpp
        src/editor/shell/toolbar.cpp
        src/editor/shell/viewport.cpp
        src/editor/ui/appearance.cpp
        src/editor/ui/fonts.cpp
        src/editor/ui/theme.cpp
        src/editor/ui/toasts.cpp
        src/editor/ui/widgets_asset.cpp
        src/editor/ui/widgets_controls.cpp
        src/editor/ui/widgets_property.cpp
        src/editor/vendor/imgui_impl_sdl3.cpp)
    # engine_scene is PUBLIC: editor.hpp exposes entt entity types (selection)
    # and the hierarchy/inspector panels reflect scene components.
    # imgui-node-editor (Behaviour panel, G2) comes before imgui::imgui: its
    # vcpkg target does not declare the imgui dependency, so a single-pass
    # linker must see it first to resolve its ImGui references.
    target_link_libraries(engine_editor
        PUBLIC engine_flags engine_scene engine_script engine_project
        PRIVATE engine_editor_build
                unofficial::imgui-node-editor::imgui-node-editor
                imgui::imgui imguizmo::imguizmo SDL3::SDL3)
    target_include_directories(engine_editor PUBLIC ${ENGINE_PUBLIC_INCLUDE})
    # Vendored third-party backend: not warning-clean under our strict flags.
    set_source_files_properties(src/editor/vendor/imgui_impl_sdl3.cpp
        PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
endif()

# ---------------------------------------------------------------------------
# game — executable entry point. The full target graph (engine_core /
# engine_renderer / engine_editor) grows phase by phase as modules gain sources.
# ---------------------------------------------------------------------------
# src/app/ is the application layer (Application owns the window, device and
# frame loop; FrameRenderer, WorldRuntime, FlyCamera, EditorBridge, the remote
# bridge and the startup scenes run inside it); it is not an engine library.
# Two executables share it (P4): `game` (the editor build when ENGINE_EDITOR is
# on) and `quat-player`, the runtime player games ship with — always built
# without the editor. Each entry point only parses the environment and hands
# off to Application.
#
# game_world holds the window-free part of the app layer — WorldRuntime (the
# simulated world: demo scenes, terrain, grass, particles, navmesh, audio,
# play mode and gameplay scripts) — so tests can drive the exact frame-loop
# simulation path (tests/app/, including the play state machine) without SDL.
# world_runtime.cpp has editor-only behaviour (script hot reload on, the play
# session's physics world), so an editor build compiles it twice: game_world
# with ENGINE_EDITOR for `game`, game_world_runtime without it for the players.
set(GAME_WORLD_SOURCES
    src/app/world_runtime.cpp
    src/app/demo_scenes.cpp
    src/app/audio_runtime.cpp
    src/app/play_mode.cpp
    src/app/grass_runtime.cpp
    src/app/particle_runtime.cpp
    src/app/nav_bake.cpp
    src/app/input_runtime.cpp)
set(GAME_WORLD_LIBRARIES
    engine_flags engine_core engine_input engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_audio engine_script)
add_library(game_world STATIC ${GAME_WORLD_SOURCES})
target_link_libraries(game_world PUBLIC ${GAME_WORLD_LIBRARIES})
target_link_libraries(game_world PRIVATE engine_editor_build)
target_include_directories(game_world PUBLIC
    ${ENGINE_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(ENGINE_EDITOR)
    add_library(game_world_runtime STATIC ${GAME_WORLD_SOURCES})
    target_link_libraries(game_world_runtime PUBLIC ${GAME_WORLD_LIBRARIES})
    target_include_directories(game_world_runtime PUBLIC
        ${ENGINE_PUBLIC_INCLUDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
else()
    add_library(game_world_runtime ALIAS game_world)
endif()

set(QUAT_APP_SOURCES
    src/app/application.cpp
    src/app/app_config.cpp
    src/app/build_service.cpp
    src/app/fly_camera.cpp
    src/app/frame_graph.cpp
    src/app/frame_temporal.cpp
    src/app/frame_post.cpp
    src/app/frame_environment.cpp
    src/app/frame_renderer.cpp
    src/app/frame_profiling.cpp
    src/app/game_boot.cpp
    src/app/game_startup.cpp
    src/app/input_sampler.cpp
    src/app/remote_bridge.cpp
    src/app/screenshot_capture.cpp
    src/app/window_icon.cpp
    src/app/asset_root.cpp)
set(QUAT_APP_LIBRARIES
    engine_flags engine_core engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_remote engine_remote_world engine_audio
    engine_script engine_project SDL3::SDL3)

add_executable(game src/main.cpp ${QUAT_APP_SOURCES})
target_link_libraries(game PRIVATE game_world ${QUAT_APP_LIBRARIES} engine_editor_build)
if(MSVC)
    # Linker map for the game_has_no_texture_encoder test.
    target_link_options(game PRIVATE "/MAP:$<TARGET_FILE_DIR:game>/game.map")
endif()
if(ENGINE_EDITOR)
    target_sources(game PRIVATE src/app/editor_bridge.cpp)
    target_link_libraries(game PRIVATE engine_editor)
endif()

add_executable(quat-player src/player/main.cpp ${QUAT_APP_SOURCES})
target_link_libraries(quat-player PRIVATE game_world_runtime ${QUAT_APP_LIBRARIES})

# quat-player-shipping: what `quat-pack --config shipping` ships. Built with
# QUAT_SHIPPING, which compiles remote control out (no endpoint, no remote
# screenshots, no QUAT_GAME override): it does not link engine_remote /
# engine_remote_world at all, so any remote reference left in the shipping
# path fails the link, and the quat_player_shipping_has_no_remote test checks
# the symbol table.
set(QUAT_SHIPPING_APP_SOURCES ${QUAT_APP_SOURCES})
list(REMOVE_ITEM QUAT_SHIPPING_APP_SOURCES
    src/app/screenshot_capture.cpp # remote render.screenshot only
    src/app/build_service.cpp)     # project.build / the editor's export only
set(QUAT_SHIPPING_APP_LIBRARIES ${QUAT_APP_LIBRARIES})
list(REMOVE_ITEM QUAT_SHIPPING_APP_LIBRARIES engine_remote engine_remote_world)
add_executable(quat-player-shipping src/player/main.cpp ${QUAT_SHIPPING_APP_SOURCES})
target_link_libraries(quat-player-shipping PRIVATE game_world_runtime ${QUAT_SHIPPING_APP_LIBRARIES})
target_compile_definitions(quat-player-shipping PRIVATE QUAT_SHIPPING)
if(MSVC)
    # Linker map for the quat_player_shipping_has_no_remote test.
    target_link_options(quat-player-shipping PRIVATE
        "/MAP:$<TARGET_FILE_DIR:quat-player-shipping>/quat-player-shipping.map")
endif()

# A game double-clicked on Windows opens no console window (wWinMain entry).
set_target_properties(quat-player quat-player-shipping PROPERTIES WIN32_EXECUTABLE TRUE)

foreach(app_target game quat-player quat-player-shipping)
    target_include_directories(${app_target} PRIVATE
        ${ENGINE_PUBLIC_INCLUDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # stb_image decodes the window icon (the implementation is in engine_scene).
    target_include_directories(${app_target} SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
endforeach()
# Data root, resolved at runtime (src/app/asset_root.hpp): QUAT_ASSET_ROOT, the
# executable's directory (packaged builds), then - development builds only -
# the source tree baked in here.
if(NOT ENGINE_PACKAGE_BUILD)
    set_property(SOURCE src/app/asset_root.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        QUAT_BUILD_TREE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        QUAT_BUILD_TREE_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cooked/shaders")
endif()

# ---------------------------------------------------------------------------
# engine_pack + quat-pack — game packaging (ROADMAP P4): collects the files a
# game references (through the scene codec, glTF and Lua scanners), cooks
# them with quat-cook when available, stages the data root with a manifest,
# and drives scripts/package-release.sh for the per-OS layout. Tool-side only:
# never linked into the game or the player.
# ---------------------------------------------------------------------------
add_library(engine_pack STATIC
    src/pack/digest.cpp
    src/pack/lua_refs.cpp
    src/pack/gltf_refs.cpp
    src/pack/dependencies.cpp
    src/pack/stage.cpp
    src/pack/pack_cli.cpp)
target_link_libraries(engine_pack PUBLIC engine_flags engine_core engine_project engine_scene)
target_link_libraries(engine_pack PRIVATE OpenSSL::Crypto)
target_include_directories(engine_pack PUBLIC ${ENGINE_PUBLIC_INCLUDE})

add_executable(quat-pack tools/quat-pack/main.cpp)
target_link_libraries(quat-pack PRIVATE engine_flags engine_pack)
target_compile_definitions(quat-pack PRIVATE QUAT_VERSION="${PROJECT_VERSION}")
# Development builds find the cooked shaders and the layout scripts in the
# source tree; packaged builds take --shaders / --engine-root.
if(NOT ENGINE_PACKAGE_BUILD)
    target_compile_definitions(quat-pack PRIVATE
        QUAT_BUILD_TREE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        QUAT_BUILD_TREE_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cooked/shaders")
endif()

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
