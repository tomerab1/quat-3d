# Game Engine — CLAUDE.md

## Project Overview
A modular, data-driven C++23 game engine with a Vulkan 1.3 renderer, Slang shaders,
glTF 2.0 skeletal animation, physics integration, and ImGui tooling. The goal is a
**high-quality, production-usable engine** — correct, well-architected, and performant.
Favour established, industry-standard techniques and best practices (the same algorithms
a shipping engine would use), not quick approximations or shortcuts. Code should be
clean, maintainable, and properly optimised; correctness and quality come first, and we
do not take shortcuts that trade away visual fidelity, performance, or sound architecture.
When there is a "proper" way and a "cheap" way to do something, default to the proper way.

---

## Development Process

See **`PHASES.md`** for the full phased plan. Work is divided into phases and slices.
**Each slice ends with a git commit** using the format `[PhaseN/SliceM] <description>`.

Rules:
- Complete and commit one slice before starting the next.
- Build must be warning-free before committing.
- Update the checkbox in `PHASES.md` for the completed slice as part of its commit.
- Never implement functionality that belongs to a later phase.
- If blocked, stop and report — do not skip ahead.

---

## Tech Stack

| Domain           | Library / API                          | Notes                                      |
|------------------|----------------------------------------|--------------------------------------------|
| Language         | C++23                                  | Modules, std::expected, std::flat_map      |
| Build            | CMake 3.28+ + vcpkg                    | vcpkg manifest mode (`vcpkg.json`)         |
| Graphics API     | Vulkan 1.3                             | Dynamic rendering, descriptor buffer       |
| Shaders          | Slang                                  | Compiled offline via `slangc` → SPIR-V     |
| glTF loading     | fastgltf                               | Async asset loading, sparse accessor support|
| Physics          | Jolt Physics                           | Discrete + continuous collision, constraints|
| UI               | Dear ImGui (docking branch)            | Vulkan + SDL3 backend                      |
| Windowing/Input  | SDL3                                   | Gamepad, mouse, keyboard unified           |
| Math             | glm (column-major, depth [0,1])        | GLM_FORCE_DEPTH_ZERO_TO_ONE always set     |
| ECS              | entt                                   | Sparse-set, view/group queries             |
| Logging          | spdlog                                 | Structured, async sink in release          |
| Profiling        | Tracy                                  | Zone macros, GPU zones via Vulkan ext      |

**Vulkan extensions assumed available:**
- `VK_KHR_dynamic_rendering`
- `VK_KHR_descriptor_buffer`
- `VK_KHR_synchronization2`
- `VK_EXT_mesh_shader` (optional, guard with feature check)
- `VK_KHR_acceleration_structure` + `VK_KHR_ray_tracing_pipeline` (optional)

---

## Repository Layout

```
engine/
├── CLAUDE.md
├── CMakeLists.txt
├── vcpkg.json
├── assets/                        # Raw source assets (not shipped)
│   ├── meshes/
│   ├── textures/
│   └── shaders/                   # .slang source files
├── cooked/                        # Offline-cooked binary assets (gitignored)
├── src/
│   ├── core/                      # Platform, logging, allocators, thread pool
│   ├── rhi/                       # Thin Vulkan abstraction (RHI)
│   │   ├── device.cpp/.hpp
│   │   ├── swapchain.cpp/.hpp
│   │   ├── pipeline_cache.cpp/.hpp
│   │   └── render_graph.cpp/.hpp
│   ├── renderer/                  # High-level renderer built on RHI
│   │   ├── mesh_pass.cpp/.hpp
│   │   ├── shadow_pass.cpp/.hpp
│   │   ├── skinning_pass.cpp/.hpp # GPU skinning compute
│   │   └── imgui_pass.cpp/.hpp
│   ├── animation/
│   │   ├── skeleton.cpp/.hpp      # Joint hierarchy, bind pose
│   │   ├── clip.cpp/.hpp          # Sampled animation clip
│   │   ├── animator.cpp/.hpp      # State machine + blend tree
│   │   └── ik/                    # FABRIK solver (optional)
│   ├── physics/
│   │   ├── physics_world.cpp/.hpp # Jolt BroadPhase + NarrowPhase wrapper
│   │   ├── collider.cpp/.hpp      # Shape descriptors
│   │   └── character.cpp/.hpp     # JPH::CharacterVirtual wrapper
│   ├── scene/
│   │   ├── scene.cpp/.hpp         # ECS world + system scheduler
│   │   ├── components.hpp         # All component structs (POD preferred)
│   │   └── gltf_loader.cpp/.hpp   # fastgltf → engine asset pipeline
│   ├── asset/
│   │   ├── asset_manager.cpp/.hpp # Async load, ref-counted handles
│   │   ├── mesh_asset.cpp/.hpp
│   │   ├── texture_asset.cpp/.hpp
│   │   └── material_asset.cpp/.hpp
│   ├── editor/                    # ImGui-based editor tools
│   │   ├── editor.cpp/.hpp
│   │   ├── scene_hierarchy.cpp/.hpp
│   │   ├── inspector.cpp/.hpp
│   │   ├── asset_browser.cpp/.hpp
│   │   └── animation_preview.cpp/.hpp
│   └── main.cpp
└── include/                       # Public headers mirroring src/ structure
    └── engine/
        ├── core/
        ├── rhi/
        ├── renderer/
        ├── animation/
        ├── physics/
        ├── scene/
        └── asset/
```

---

## Coding Conventions

### General
- **No exceptions, no RTTI.** Use `std::expected<T, Error>` for fallible operations.
- **No raw owning pointers.** Use `std::unique_ptr`, `std::shared_ptr`, or arena allocators.
- `snake_case` for everything. `SCREAMING_SNAKE_CASE` for macros only.
- `PascalCase` for types and concepts.
- Forward-declare in headers; include in `.cpp` files.
- Mark all implementation-private functions `static` or put them in anonymous namespaces.
- Prefer `[[nodiscard]]` on any function returning a status or resource.

### Headers
- Every public header in `include/engine/<module>/` — no implementation details leak.
- Private headers stay in `src/<module>/`.
- Use `#pragma once`.

### Error handling pattern
```cpp
// Prefer this over throw
auto result = create_pipeline(desc);
if (!result) {
    spdlog::error("Pipeline creation failed: {}", result.error().message);
    return std::unexpected(result.error());
}
auto& pipeline = *result;
```

### Vulkan conventions
- All Vulkan handles wrapped — never store raw `VkImage`, `VkBuffer` etc. in components.
- Every allocation goes through a `GpuAllocator` (VMA-backed).
- Synchronization via `VK_KHR_synchronization2` barrier API only — no legacy `vkCmdPipelineBarrier`.
- Render passes use `VK_KHR_dynamic_rendering` — no `VkRenderPass`/`VkFramebuffer` objects.
- Descriptor sets managed via `VK_KHR_descriptor_buffer` — no `vkAllocateDescriptorSets`.
- Pipeline layouts use push constants for per-draw data (transform index, material index).
- Frame-in-flight count: 2. All per-frame resources double-buffered.

### Slang shader conventions
- One `.slang` file per pass (e.g. `mesh.slang`, `skinning.slang`, `shadow.slang`).
- Use Slang `interface` + `struct` implementing it for material variation — no `#ifdef` soup.
- Uniform/structured buffers bound via `ParameterBlock<T>`.
- Shader entry points explicitly annotated: `[shader("vertex")]`, `[shader("fragment")]`, `[shader("compute")]`.
- Output SPIR-V is stored in `cooked/shaders/` alongside a `.json` reflection file.
- Never hardcode descriptor set/binding indices — derive from Slang reflection at pipeline creation.

---

## ECS / Component Design

- **Prefer plain POD components.** If a component needs logic, that logic lives in a system, not the component.
- Group components by access pattern for cache efficiency. Use `entt::group` when two components are always read together.
- Tag types (zero-size structs) are fine: `struct Dirty {}`, `struct Selected {}`.
- Systems are free functions or stateless structs — not classes with member state.
- System execution order is explicit: the `Scene` class registers systems in a fixed sequence each tick.

### Core components (defined in `components.hpp`)
```cpp
struct Transform    { glm::mat4 local; glm::mat4 world; };
struct MeshRenderer { AssetHandle<MeshAsset> mesh; AssetHandle<MaterialAsset> material; };
struct SkinnedMesh  { AssetHandle<SkeletonAsset> skeleton; std::vector<glm::mat4> joint_matrices; };
struct Animator     { AssetHandle<AnimClipAsset> clip; float time; float speed; bool looping; };
struct RigidBody    { JPH::BodyID body_id; bool is_kinematic; };
struct Collider     { JPH::ShapeRefC shape; glm::vec3 offset; };
struct Camera       { float fov_y; float near_z; float far_z; bool is_active; };
struct DirectionalLight { glm::vec3 direction; glm::vec3 color; float intensity; };
struct PointLight   { glm::vec3 color; float radius; float intensity; };
struct Name         { std::string value; };   // editor only, not hot path
struct Parent       { entt::entity entity; };
struct Children     { std::vector<entt::entity> entities; };
```

---

## glTF 2.0 / Skeletal Animation Pipeline

### Loading (fastgltf)
1. `GltfLoader::load(path)` parses with `fastgltf::Parser` using `fastgltf::Options::LoadExternalBuffers`.
2. For each mesh primitive: extract positions, normals, UVs, tangents, and if skinned: `JOINTS_0`, `WEIGHTS_0`.
3. Upload vertex + index data to device-local `GpuBuffer` via staging.
4. For each skin: build `SkeletonAsset` — joint names, parent indices, inverse bind matrices.
5. For each animation: build `AnimClipAsset` — per-channel samplers (translation/rotation/scale), min/max times.

### Skeleton runtime
- `Skeleton` stores joint hierarchy as a flat array with parent index (-1 for root).
- Bind pose stored as local TRS per joint.
- `Animator::update(dt)` samples the clip and writes local TRS, then `compute_world_transforms()` walks the hierarchy to produce world-space joint matrices.
- Skinning matrices = `joint_world * inverse_bind`.

### GPU skinning
- Skinning runs in a **compute pass** before the mesh pass (not vertex shader skinning).
- Input: original vertex buffer (positions + blend weights/indices), joint matrices SSBO.
- Output: skinned vertex buffer written in place (double-buffer if needed).
- This keeps the mesh pass vertex shader simple and avoids joint matrix uniforms per draw.

### Animation blending
- `BlendTree` supports: single clip, linear 1D blend (walk/run), additive blend (layer system).
- Blend weights normalized before applying. Additive layers masked per joint by a bitmask.
- State machine (`AnimStateMachine`) has states and parameterized transitions with crossfade duration.

---

## Physics (Jolt)

- `PhysicsWorld` owns a `JPH::PhysicsSystem` with a fixed timestep of `1/120s`, accumulated.
- Collision layers: `STATIC(0)`, `DYNAMIC(1)`, `CHARACTER(2)`, `TRIGGER(3)`, `SENSOR(4)`.
- `RigidBody` component stores only the `JPH::BodyID` — Jolt owns the body data.
- On scene tick: sync `Transform` → Jolt body (for kinematic), then step, then sync Jolt → `Transform` (for dynamic).
- Character controller uses `JPH::CharacterVirtual` — no rigid body, fully predictive.
- Debug rendering: Jolt's `DebugRenderer` interface implemented to emit ImGui draw list lines (editor only).

### Shape creation helpers
```
collider_box(half_extents)     → JPH::BoxShape
collider_sphere(radius)        → JPH::SphereShape
collider_capsule(half_h, r)    → JPH::CapsuleShape
collider_mesh(positions, idx)  → JPH::MeshShape   (static only)
collider_convex(positions)     → JPH::ConvexHullShape
```

---

## Render Graph

- `RenderGraph` is rebuilt each frame from a declarative description (not cached — graph compilation is fast).
- Each `RenderPass` declares: read resources, write resources, execute lambda.
- Graph compiler: topological sort → barrier insertion → execution.
- All transient images allocated from a per-frame pool; only persistent images (shadow maps, GBuffer) live beyond a frame.
- Pass types: `GraphicsPass`, `ComputePass`, `TransferPass`.

### Frame structure (default)
```
[Compute]   SkinningPass         — GPU skinning for all SkinnedMesh entities
[Graphics]  ShadowPass           — Directional light cascaded shadow map (4 cascades)
[Graphics]  GBufferPass          — Deferred: albedo, normal, material, depth
[Compute]   LightingPass         — Tiled/clustered lighting → HDR color buffer
[Graphics]  SkyPass              — Procedural sky or HDRI (rendered to HDR buffer)
[Graphics]  TransparentPass      — Forward+ alpha-blended geometry
[Compute]   PostProcessPass      — TAA, bloom, exposure
[Graphics]  TonemapPass          — ACES/AgX → swapchain image (LDR)
[Graphics]  ImGuiPass            — Editor UI overlay
```

---

## Asset Manager

- Assets identified by `AssetId` (stable hash of relative path).
- `AssetHandle<T>` is a ref-counted, nullable handle. Dereferencing a not-yet-loaded handle returns a default asset.
- Load requests dispatched to a thread pool; completion written via `std::atomic` flag on the handle.
- Hot-reload: file watcher triggers re-cook + re-upload when source changes (editor builds only).
- Cooked format: flatbuffers schema per asset type for zero-copy mmap on load.

---

## Editor (ImGui)

- Editor runs as a **layer** on top of the engine — not baked in. `#ifdef ENGINE_EDITOR` guards all editor code in non-editor builds.
- ImGui docking layout with panels:
  - **Scene Hierarchy** — tree view of entities, drag to re-parent, right-click context menu.
  - **Inspector** — reflect all components via a `ComponentInspector<T>` template specialisation.
  - **Asset Browser** — filesystem tree under `assets/`, thumbnail previews, drag-to-scene.
  - **Animation Preview** — timeline scrubber, blend weight sliders, joint hierarchy overlay.
  - **Viewport** — scene rendered to an offscreen image, displayed as `ImGui::Image`.
  - **Physics Debug** — toggle Jolt debug draw, collision shape overlay.
  - **Profiler** — Tracy frame graph embedded (or link to Tracy standalone).
- Editor state (selected entity, camera transform, panel layout) is NOT stored in ECS — it lives in `EditorState` struct.
- Gizmos: ImGuizmo for translate/rotate/scale handles in the viewport.

---

## Threading Model

- **Main thread**: SDL event loop, ECS tick, render graph submission.
- **Thread pool** (`std::jthread` × N-1 cores): asset loading, physics step (Jolt has its own job system — hand it the thread pool).
- **No engine-level mutexes on hot paths.** Systems run single-threaded in declared order; parallelism is within Jolt and the asset loader.
- Tracy zones on all systems and passes. GPU zones on all render/compute passes.

---

## Build & CMake Notes

- vcpkg dependencies declared in `vcpkg.json`. Key ports: `fastgltf`, `entt`, `glm`, `spdlog`, `imgui[docking-experimental,vulkan-binding,sdl3-binding]`, `joltphysics`, `tracy`, `vulkan-memory-allocator`, `imguizmo`.
- Slang is not in vcpkg — fetch via `FetchContent` from the official GitHub release.
- CMake targets:
  - `engine_core` — static lib, no graphics
  - `engine_rhi` — static lib, Vulkan only
  - `engine_renderer` — static lib, depends on rhi + core
  - `engine_editor` — static lib, compiled only with `-DENGINE_EDITOR=ON`
  - `game` — executable, links all
- Compile options: `-Wall -Wextra -Wshadow`, `/W4` on MSVC. Release: `-O2 -DNDEBUG`. Tracy disabled in release unless `ENGINE_PROFILE=ON`.
- Shader compilation invoked as a CMake custom command: `slangc assets/shaders/mesh.slang -target spirv -o cooked/shaders/mesh.spv -reflection-json cooked/shaders/mesh.json`.

---

## What Claude Should Never Do

- Never use `VkRenderPass` or `VkFramebuffer` — we use dynamic rendering.
- Never use `vkAllocateDescriptorSets` or descriptor pools — we use descriptor buffer.
- Never store raw Vulkan handles (`VkImage`, `VkBuffer`) in components or assets — always wrapped types.
- Never allocate GPU memory with `vkAllocateMemory` directly — always VMA.
- Never write skinning in the vertex shader — skinning is a compute pass.
- Never use `#ifdef` for material/shader variants — use Slang interfaces.
- Never put system logic inside component structs.
- Never throw exceptions.
- Never use `new`/`delete` directly — prefer smart pointers or arena allocators.
- Never include ImGui headers outside `src/editor/` and `src/renderer/imgui_pass.cpp`.
