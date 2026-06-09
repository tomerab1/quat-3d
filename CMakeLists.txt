# quat-3d game engine — root build script.
#
# Toolchain: the project requires a C++23 standard library with <expected> and
# <flat_map>. On this machine only clang++ with libc++ provides both, so the
# presets pin CMAKE_CXX_COMPILER=clang++ and -stdlib=libc++. See CMakePresets.json.
#
# Note: CLAUDE.md targets CMake 3.28+, but the installed toolchain ships 3.22, so
# the minimum is pinned to 3.22. Raise this once a newer CMake is available.
cmake_minimum_required(VERSION 3.22)

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

# ---------------------------------------------------------------------------
# Common compile options applied to all first-party engine targets.
# Third-party (vcpkg) targets are unaffected.
# ---------------------------------------------------------------------------
add_library(engine_flags INTERFACE)

target_compile_features(engine_flags INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(engine_flags INTERFACE /W4 /permissive- /EHs-c- /GR-)
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

set(ENGINE_PUBLIC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# engine_rhi — thin Vulkan abstraction (instance/device, swapchain, ...).
# ---------------------------------------------------------------------------
add_library(engine_rhi STATIC
    src/rhi/device.cpp
    src/rhi/swapchain.cpp
    src/rhi/gpu_allocator.cpp
    src/rhi/vma_impl.cpp
    src/rhi/shader_reflection.cpp
    src/rhi/pipeline_cache.cpp
    src/rhi/descriptor_buffer.cpp
    src/rhi/render_graph.cpp
    src/rhi/graphics_pipeline.cpp
    src/rhi/compute_pipeline.cpp)
target_link_libraries(engine_rhi PUBLIC
    engine_flags Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
# nlohmann_json is an implementation detail of the reflection parser (PRIVATE).
target_link_libraries(engine_rhi PRIVATE nlohmann_json::nlohmann_json)
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
    src/asset/asset_manager.cpp
    src/asset/material_asset.cpp)
target_link_libraries(engine_asset PUBLIC engine_flags engine_rhi glm::glm)
target_include_directories(engine_asset PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# engine_animation — CPU-side skeletal animation (skeletons, clips, animator).
# Graphics-free: plain data + math, consumed by engine_scene and the renderer's
# GPU skinning pass. Starts with SkeletonAsset (5.1).
# ---------------------------------------------------------------------------
add_library(engine_animation STATIC
    src/animation/skeleton.cpp
    src/animation/clip.cpp
    src/animation/animator.cpp
    src/animation/state_machine.cpp)
target_link_libraries(engine_animation PUBLIC engine_flags glm::glm)
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
    src/scene/gltf_loader.cpp
    src/scene/stb_image_impl.cpp
    src/scene/components.cpp
    src/scene/scene.cpp
    src/scene/physics_system.cpp)
# EnTT is PUBLIC: components.hpp exposes entt types (Parent/Children) to consumers.
# engine_renderer is PUBLIC: scene.hpp's RenderCollectSystem fills a renderer
# DrawItem list (no cycle — the renderer does not depend on the scene).
# engine_animation is PUBLIC: components.hpp's SkinnedMesh holds a SkeletonAsset
# handle and the glTF loader extracts skins.
target_link_libraries(engine_scene PUBLIC
    engine_flags engine_asset engine_rhi engine_renderer engine_animation engine_physics EnTT::EnTT)
# fastgltf and stb (image decode) are implementation details of the loader.
target_link_libraries(engine_scene PRIVATE fastgltf::fastgltf)
target_include_directories(engine_scene PRIVATE ${Stb_INCLUDE_DIR})
target_include_directories(engine_scene PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# stb's implementation TU is not warning-clean under our strict flags.
set_source_files_properties(src/scene/stb_image_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---------------------------------------------------------------------------
# engine_renderer — high-level passes built on the RHI. Starts with the deferred
# GBuffer pass (mesh_pass); lighting/tonemap/imgui passes follow in later slices.
# ---------------------------------------------------------------------------
add_library(engine_renderer STATIC
    src/renderer/mesh_pass.cpp
    src/renderer/lighting_pass.cpp
    src/renderer/tonemap_pass.cpp
    src/renderer/skinning_pass.cpp
    src/renderer/shadow_pass.cpp)
target_link_libraries(engine_renderer PUBLIC engine_flags engine_rhi engine_asset glm::glm)
target_include_directories(engine_renderer PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# game — executable entry point. The full target graph (engine_core /
# engine_renderer / engine_editor) grows phase by phase as modules gain sources.
# ---------------------------------------------------------------------------
add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE
    engine_flags engine_rhi engine_asset engine_scene engine_renderer engine_physics SDL3::SDL3)
target_include_directories(game PRIVATE
    ${ENGINE_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Absolute path to cooked shaders so the binary finds them regardless of CWD.
# A proper asset-path resolver arrives with the asset manager in Phase 3.
target_compile_definitions(game PRIVATE
    QUAT_COOKED_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cooked/shaders")

# ---------------------------------------------------------------------------
# anim_smoke — windowless CPU harness for the animation self-tests (no SDL /
# Vulkan), so the animation runtime can be verified without a display.
# ---------------------------------------------------------------------------
add_executable(anim_smoke tests/anim_smoke.cpp)
target_link_libraries(anim_smoke PRIVATE engine_flags engine_animation)
target_include_directories(anim_smoke PRIVATE ${ENGINE_PUBLIC_INCLUDE})

# ---------------------------------------------------------------------------
# Offline shader cooking: assets/shaders/*.slang -> cooked/shaders/*.{spv,json}
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)
add_shader_compilation(shaders)
add_dependencies(game shaders)
