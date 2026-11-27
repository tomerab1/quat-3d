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
option(ENGINE_PROFILE "Enable Tracy profiling in non-debug builds" OFF)
# Compatibility switch for drivers without VK_EXT_descriptor_buffer (MoltenVK on
# macOS). When ON, the RHI may fall back to classic descriptor pools/sets and the
# instance opts into portability (non-conformant) ICD enumeration. Descriptor
# buffers remain the mandated model (CLAUDE.md); this exists only so the engine
# can run on platforms where the extension does not exist.
option(ENGINE_DESCRIPTOR_SETS_FALLBACK
    "Allow classic descriptor-set fallback on drivers without VK_EXT_descriptor_buffer" OFF)

# ---------------------------------------------------------------------------
# Common compile options applied to all first-party engine targets.
# Third-party (vcpkg) targets are unaffected.
# ---------------------------------------------------------------------------
add_library(engine_flags INTERFACE)

target_compile_features(engine_flags INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(engine_flags INTERFACE /W4 /permissive- /EHs-c- /GR-)
    # getenv/fopen are used deliberately (portable C, bounds-checked call
    # sites); _HAS_EXCEPTIONS=0 matches /EHs-c- so the STL stops emitting
    # C4530 from its internal try/catch.
    target_compile_definitions(engine_flags INTERFACE
        _CRT_SECURE_NO_WARNINGS
        _HAS_EXCEPTIONS=0)
else()
    target_compile_options(engine_flags INTERFACE
        -Wall -Wextra -Wshadow
        -fno-exceptions -fno-rtti)
endif()

# Per CLAUDE.md: column-major, reverse-Z friendly depth range [0, 1].
target_compile_definitions(engine_flags INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    $<$<CONFIG:Release>:NDEBUG>)

if(ENGINE_EDITOR)
    target_compile_definitions(engine_flags INTERFACE ENGINE_EDITOR)
endif()

if(ENGINE_DESCRIPTOR_SETS_FALLBACK)
    target_compile_definitions(engine_flags INTERFACE ENGINE_DESCRIPTOR_SETS_FALLBACK)
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
find_package(Stb REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(recastnavigation CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
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
if(ENGINE_EDITOR)
    find_package(imgui CONFIG REQUIRED)
    find_package(imguizmo CONFIG REQUIRED)
    find_package(unofficial-imgui-node-editor CONFIG REQUIRED)
endif()

set(ENGINE_PUBLIC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# engine_core — graphics-free foundations: the log ring + log() helper, stderr
# capture into the ring, the simulation clock (A5; S6 grows the logging layer
# here), file watching and child processes (shader hot reload, R2).
# ---------------------------------------------------------------------------
add_library(engine_core STATIC
    src/core/log.cpp
    src/core/stderr_capture.cpp
    src/core/sim_clock.cpp
    src/core/file_watcher.cpp
    src/core/child_process.cpp)
target_link_libraries(engine_core PUBLIC engine_flags Threads::Threads)
target_include_directories(engine_core PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_rhi — thin Vulkan abstraction (instance/device, swapchain, ...).
# ---------------------------------------------------------------------------
add_library(engine_rhi STATIC
    src/rhi/device.cpp
    src/rhi/queue_selection.cpp
    src/rhi/swapchain.cpp
    src/rhi/gpu_allocator.cpp
    src/rhi/vma_impl.cpp
    src/rhi/shader_reflection.cpp
    src/rhi/pipeline_cache.cpp
    src/rhi/descriptor_buffer.cpp
    src/rhi/render_graph.cpp
    src/rhi/graphics_pipeline.cpp
    src/rhi/compute_pipeline.cpp
    src/rhi/shader_compiler.cpp
    src/rhi/shader_hot_reload.cpp)
target_link_libraries(engine_rhi PUBLIC
    engine_flags engine_core Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
# engine_core is public: shader hot reload's headers use its watcher/process
# types, and the debug messenger routes validation messages into its log ring.
# The hot-reload compile worker runs on a std::thread.
target_link_libraries(engine_rhi PRIVATE Threads::Threads)
# nlohmann_json is an implementation detail of the reflection parser (PRIVATE);
# glm because the parser reads slangc's JSON through core::JsonFields.
target_link_libraries(engine_rhi PRIVATE nlohmann_json::nlohmann_json glm::glm)
target_include_directories(engine_rhi PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# VMA's implementation is not warning-clean under our strict flags; compile just
# that TU permissively (our code using VMA stays strict).
set_source_files_properties(src/rhi/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---------------------------------------------------------------------------
# engine_asset — asset identity, ref-counted handles, synchronous load path,
# and asset types (MeshAsset). MeshAsset owns GpuBuffers, so engine_rhi and glm
# are PUBLIC: any consumer of the asset headers needs them transitively.
# ---------------------------------------------------------------------------
add_library(engine_asset STATIC
    src/asset/alpha_coverage.cpp
    src/asset/asset_manager.cpp
    src/asset/material_asset.cpp)
target_link_libraries(engine_asset PUBLIC engine_flags engine_rhi glm::glm)
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
target_link_libraries(engine_animation PRIVATE acl_headers)
target_include_directories(engine_animation PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_physics — Jolt Physics wrapper (PhysicsWorld). Jolt is an
# implementation detail (PRIVATE); the public header is Jolt-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_physics STATIC
    src/physics/physics_world.cpp)
target_link_libraries(engine_physics PUBLIC engine_flags)
target_link_libraries(engine_physics PRIVATE Jolt::Jolt)
target_include_directories(engine_physics PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_scene — scene-construction side of the asset pipeline. For now just the
# glTF loader (fastgltf -> MeshAsset); gains the ECS world + systems in Phase 4.
# fastgltf is an implementation detail of the loader (PRIVATE).
# ---------------------------------------------------------------------------
add_library(engine_scene STATIC
    src/scene/gltf/validate.cpp
    src/scene/gltf/mesh.cpp
    src/scene/gltf/material.cpp
    src/scene/gltf/skin_animation.cpp
    src/scene/gltf/clip_compression.cpp
    src/scene/gltf/scene_assembly.cpp
    src/scene/gltf/self_tests.cpp
    src/scene/stb_image_impl.cpp
    src/scene/components.cpp
    src/scene/scene.cpp
    src/scene/animation_system.cpp
    src/scene/physics_system.cpp
    src/scene/nav_agent_system.cpp
    src/scene/navigation.cpp
    src/scene/behavior.cpp
    src/scene/bt_graph.cpp
    src/scene/bt_graph_edit.cpp
    src/scene/scene_io.cpp
    src/scene/component_json.cpp
    src/scene/component_json_animation.cpp
    src/scene/material_override.cpp
    src/scene/render_settings_json.cpp
    src/scene/audio_system.cpp
    src/scene/primitives.cpp
    src/scene/primitive_resolve.cpp
    src/scene/scatter.cpp
    src/scene/instanced_mesh.cpp
    src/scene/instanced_mesh_resolve.cpp)
# EnTT is PUBLIC: components.hpp exposes entt types (Parent/Children) to consumers.
# engine_renderer is PUBLIC: scene.hpp's RenderCollectSystem fills a renderer
# DrawItem list (no cycle — the renderer does not depend on the scene).
# engine_animation is PUBLIC: components.hpp's SkinnedMesh holds a SkeletonAsset
# handle and the glTF loader extracts skins.
target_link_libraries(engine_scene PUBLIC
    engine_flags engine_asset engine_rhi engine_renderer engine_animation engine_physics
    engine_terrain engine_nav engine_audio EnTT::EnTT)
# fastgltf and stb (image decode) are implementation details of the loader.
# nlohmann_json is PUBLIC: component_json.hpp exposes JSON values (the codec
# shared by scene files and the remote-control API).
target_link_libraries(engine_scene PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(engine_scene PRIVATE fastgltf::fastgltf)
target_include_directories(engine_scene PRIVATE ${Stb_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(engine_scene PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# stb's implementation TU is not warning-clean under our strict flags.
set_source_files_properties(src/scene/stb_image_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---------------------------------------------------------------------------
# engine_terrain — procedural terrain (Phase 12). Pure CPU heightmap generation
# in 12.1; chunked LOD rendering and Jolt height fields layer on in 12.2/12.3.
# ---------------------------------------------------------------------------
add_library(engine_terrain STATIC
    src/terrain/generator.cpp
    src/terrain/grid_erosion.cpp
    src/terrain/streamer.cpp)
target_link_libraries(engine_terrain PUBLIC engine_flags glm::glm)
target_include_directories(engine_terrain PUBLIC ${ENGINE_PUBLIC_INCLUDE})

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
target_link_libraries(engine_audio PRIVATE miniaudio_headers)
target_include_directories(engine_audio PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# miniaudio's implementation is not warning-clean under our strict flags.
set_source_files_properties(src/audio/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---------------------------------------------------------------------------
# engine_remote — remote-control endpoint (Phase 15): loopback JSON-RPC 2.0
# over TCP, poll-driven on the main thread. Out-of-process tools (the quat-mcp
# Model Context Protocol bridge in tools/quat-mcp) drive the engine through it.
# nlohmann_json is PUBLIC: the handler API traffics in JSON values.
# ---------------------------------------------------------------------------
add_library(engine_remote STATIC
    src/remote/rpc_server.cpp
    src/remote/socket.cpp)
target_link_libraries(engine_remote PUBLIC engine_flags nlohmann_json::nlohmann_json)
target_include_directories(engine_remote PUBLIC ${ENGINE_PUBLIC_INCLUDE})
if(WIN32)
    target_link_libraries(engine_remote PRIVATE ws2_32)
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
    src/remote/world/camera_commands.cpp
    src/remote/world/app_commands.cpp
    src/remote/world/debug_commands.cpp
    src/remote/world/debug_query_commands.cpp
    src/remote/world/tuning_commands.cpp
    src/remote/world/behavior_commands.cpp
    src/remote/world/behavior_edit_commands.cpp
    src/remote/world/image_encode.cpp
    src/remote/world/stb_image_write_impl.cpp)
target_link_libraries(engine_remote_world PUBLIC
    engine_flags engine_core engine_remote engine_scene engine_physics engine_nav)
target_include_directories(engine_remote_world PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_remote_world PRIVATE ${Stb_INCLUDE_DIR})
# stb's implementation TU is not warning-clean under our strict flags.
set_source_files_properties(src/remote/world/stb_image_write_impl.cpp
    PROPERTIES COMPILE_OPTIONS "-w")

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
    src/renderer/mesh_pass.cpp
    src/renderer/lighting_pass.cpp
    src/renderer/light_clusters.cpp
    src/renderer/light_cull_pass.cpp
    src/renderer/tonemap_pass.cpp
    src/renderer/skinning_pass.cpp
    src/renderer/shadow_casters.cpp
    src/renderer/shadow_pass.cpp
    src/renderer/transparent_pass.cpp
    src/renderer/ibl_pass.cpp
    src/renderer/dynamic_ibl.cpp
    src/renderer/bloom_pass.cpp
    src/renderer/exposure_pass.cpp
    src/renderer/taa_pass.cpp
    src/renderer/atmosphere_pass.cpp
    src/renderer/terrain_pass.cpp
    src/renderer/terrain_gpu_tiles.cpp
    src/renderer/terrain_synth_pass.cpp
    src/renderer/terrain_shadow.cpp)
target_link_libraries(engine_renderer PUBLIC
    engine_flags engine_rhi engine_asset engine_terrain glm::glm)
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
        src/editor/asset_browser.cpp
        src/editor/animation_preview.cpp
        src/editor/physics_debug.cpp
        src/editor/picking.cpp
        src/editor/scene_snapshot.cpp
        src/editor/audio_inspector.cpp
        src/editor/animation_inspector.cpp
        src/editor/grass_inspector.cpp
        src/editor/behavior_panel.cpp
        src/editor/behavior_canvas.cpp
        src/editor/vendor/imgui_impl_sdl3.cpp)
    # engine_scene is PUBLIC: editor.hpp exposes entt entity types (selection)
    # and the hierarchy/inspector panels reflect scene components.
    # imgui-node-editor (Behaviour panel, G2) comes before imgui::imgui: its
    # vcpkg target does not declare the imgui dependency, so a single-pass
    # linker must see it first to resolve its ImGui references.
    target_link_libraries(engine_editor
        PUBLIC engine_flags engine_scene
        PRIVATE unofficial::imgui-node-editor::imgui-node-editor
                imgui::imgui imguizmo::imguizmo SDL3::SDL3)
    target_include_directories(engine_editor PUBLIC ${ENGINE_PUBLIC_INCLUDE})
    # Vendored third-party backend: not warning-clean under our strict flags.
    set_source_files_properties(src/editor/vendor/imgui_impl_sdl3.cpp
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---------------------------------------------------------------------------
# game — executable entry point. The full target graph (engine_core /
# engine_renderer / engine_editor) grows phase by phase as modules gain sources.
# ---------------------------------------------------------------------------
# src/app/ is the application layer (Application owns the window, device and
# frame loop; FrameRenderer, WorldRuntime, FlyCamera, EditorBridge, the remote
# bridge and the startup scenes run inside it); it is not an engine library.
# main.cpp only parses the environment and hands off to Application.
add_executable(game
    src/main.cpp
    src/app/application.cpp
    src/app/fly_camera.cpp
    src/app/frame_graph.cpp
    src/app/frame_renderer.cpp
    src/app/world_runtime.cpp
    src/app/demo_scenes.cpp
    src/app/audio_runtime.cpp
    src/app/grass_runtime.cpp
    src/app/nav_bake.cpp
    src/app/remote_bridge.cpp
    src/app/screenshot_capture.cpp)
target_link_libraries(game PRIVATE
    engine_flags engine_core engine_rhi engine_asset engine_scene engine_renderer engine_physics
    engine_terrain engine_nav engine_net engine_remote engine_remote_world engine_audio
    SDL3::SDL3)
if(ENGINE_EDITOR)
    target_sources(game PRIVATE src/app/editor_bridge.cpp)
    target_link_libraries(game PRIVATE engine_editor)
endif()
target_include_directories(game PRIVATE
    ${ENGINE_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Absolute path to cooked shaders so the binary finds them regardless of CWD.
# A proper asset-path resolver arrives with the asset manager in Phase 3.
target_compile_definitions(game PRIVATE
    QUAT_COOKED_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cooked/shaders"
    QUAT_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

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
# Shader hot reload (R2) runs the same slangc the build cooks with.
target_compile_definitions(engine_rhi PRIVATE QUAT_SLANGC_PATH="${SLANGC_EXECUTABLE}")
