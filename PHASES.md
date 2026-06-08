# Engine Development Phases

This file is the source of truth for development order.
Each slice ends with a `git commit`. Do not move to the next slice until the current one
compiles cleanly, runs without validation errors, and the commit is made.

After completing every slice, update the checkbox here and commit the updated PHASES.md
as part of that slice's commit.

**Rules for Claude Code:**
- Complete one slice fully before starting the next.
- Each slice commit message must follow the format: `[PhaseN/SliceM] <description>`.
- Never implement functionality that belongs to a later phase.
- If a slice is blocked, stop and report the blocker — do not skip ahead.
- Run the build and fix all warnings before committing.

---

## Phase 1 — Foundation & Window

Goal: a window opens, Vulkan initialises, a black frame is presented, the app closes cleanly.

- [x] **1.1 — Repo scaffold**
  Create `CMakeLists.txt`, `vcpkg.json`, directory skeleton (`src/`, `include/`, `assets/`, `cooked/`),
  `.gitignore`, and this `CLAUDE.md` + `PHASES.md` at root.
  *Commit: `[Phase1/Slice1] repo scaffold and build system`*

- [x] **1.2 — SDL3 window + event loop**
  `src/main.cpp` opens an SDL3 window, pumps events, exits cleanly on close/ESC.
  No Vulkan yet.
  *Commit: `[Phase1/Slice2] SDL3 window and event loop`*

- [x] **1.3 — Vulkan device**
  `src/rhi/device.cpp`: instance, debug messenger, physical device selection (prefer discrete),
  logical device, graphics + compute + transfer queues.
  Validation layers enabled in debug builds.
  *Commit: `[Phase1/Slice3] Vulkan device creation`*

- [x] **1.4 — Swapchain + present loop**
  `src/rhi/swapchain.cpp`: swapchain, image views, per-frame sync primitives (2 frames in flight).
  Main loop acquires, submits an empty command buffer, presents. No rendering yet.
  *Commit: `[Phase1/Slice4] swapchain and present loop`*

- [x] **1.5 — VMA + GpuBuffer / GpuImage wrappers**
  Integrate VulkanMemoryAllocator. Implement `GpuBuffer` and `GpuImage` RAII wrappers.
  Add a staging helper for CPU→GPU uploads.
  *Commit: `[Phase1/Slice5] VMA and GPU resource wrappers`*

---

## Phase 2 — Render Graph & First Triangle

Goal: render graph infrastructure exists; a hard-coded triangle appears on screen via a Slang shader.

- [x] **2.1 — Slang shader compilation pipeline**
  CMake custom command runs `slangc` on all `.slang` files in `assets/shaders/`,
  outputs SPIR-V + reflection JSON to `cooked/shaders/`.
  Add a trivial `passthrough.slang` (fullscreen triangle, outputs solid colour) to verify.
  *Commit: `[Phase2/Slice1] Slang shader compilation pipeline`*

- [x] **2.2 — Pipeline cache + shader module loader**
  `src/rhi/pipeline_cache.cpp`: load SPIR-V, create `VkShaderModule`, cache by path hash.
  Parse reflection JSON to auto-derive push constant ranges and descriptor layouts.
  *Commit: `[Phase2/Slice2] pipeline cache and shader reflection`*

- [x] **2.3 — Descriptor buffer setup**
  Implement descriptor buffer allocation (one buffer per descriptor set layout).
  Helper to write sampler/image/buffer descriptors into the buffer at a given offset.
  *Commit: `[Phase2/Slice3] descriptor buffer infrastructure`*

- [x] **2.4 — Render graph core**
  `src/rhi/render_graph.cpp`: `RenderPass` declaration (reads/writes), topological sort,
  automatic `VkImageMemoryBarrier2` insertion via synchronization2, execution loop.
  Transient image pool. No actual passes yet — just the framework.
  *Commit: `[Phase2/Slice4] render graph core`*

- [x] **2.5 — First triangle**
  `assets/shaders/mesh.slang`: minimal vertex + fragment shader (hardcoded triangle, UV-based colour).
  `GraphicsPass` registered in the render graph writing to the swapchain image.
  Triangle appears on screen.
  *Commit: `[Phase2/Slice5] first triangle via render graph`*

---

## Phase 3 — Asset Pipeline & Mesh Rendering

Goal: a glTF static mesh loads and renders with a basic PBR material under a directional light.

- [x] **3.1 — Asset manager skeleton**
  `src/asset/asset_manager.cpp`: `AssetId` (xxHash64 of relative path), `AssetHandle<T>`,
  synchronous load path (async deferred to Phase 6). Default fallback assets.
  *Commit: `[Phase3/Slice1] asset manager skeleton`*

- [ ] **3.2 — glTF static mesh loading**
  `src/scene/gltf_loader.cpp` using fastgltf: load positions, normals, UVs, tangents, indices.
  Upload to device-local vertex + index buffers. Store as `MeshAsset`.
  *Commit: `[Phase3/Slice2] glTF static mesh loading`*

- [ ] **3.3 — Texture loading**
  Load glTF images (base colour, normal, metallic-roughness, emissive) via fastgltf.
  Upload to `GpuImage`, create `VkImageView` + `VkSampler`. Store as `TextureAsset`.
  *Commit: `[Phase3/Slice3] glTF texture loading`*

- [ ] **3.4 — Material asset + Slang material interface**
  Define `IMaterial` Slang interface. Implement `PbrMaterial` struct.
  `MaterialAsset` stores GPU buffer with material parameters + texture descriptors.
  *Commit: `[Phase3/Slice4] material asset and Slang material interface`*

- [ ] **3.5 — GBuffer pass**
  `assets/shaders/gbuffer.slang` + `src/renderer/mesh_pass.cpp`:
  Deferred GBuffer: albedo (RGB), normal (RG oct-encoded), material (metallic/roughness/AO), depth.
  Render all `MeshRenderer` entities.
  *Commit: `[Phase3/Slice5] deferred GBuffer pass`*

- [ ] **3.6 — Lighting pass**
  `assets/shaders/lighting.slang` + `src/renderer/lighting_pass.cpp`:
  Fullscreen compute pass reading GBuffer, applying one directional light, writing HDR colour.
  *Commit: `[Phase3/Slice6] deferred lighting pass`*

- [ ] **3.7 — Tonemap + present**
  `assets/shaders/tonemap.slang`: ACES tonemap, gamma correction, write to swapchain image.
  Static glTF mesh now visible with correct lighting.
  *Commit: `[Phase3/Slice7] tonemap pass and lit static mesh`*

---

## Phase 4 — ECS & Scene

Goal: entities with components drive rendering; a scene can be constructed in code.

- [ ] **4.1 — ECS world + core components**
  Integrate entt. Define all core components from `components.hpp` (Transform, MeshRenderer,
  Camera, DirectionalLight, Name, Parent, Children).
  *Commit: `[Phase4/Slice1] ECS world and core components`*

- [ ] **4.2 — Scene + system scheduler**
  `src/scene/scene.cpp`: owns `entt::registry`, registers and ticks systems in fixed order.
  Systems: `TransformSystem` (propagate world matrices), `RenderCollectSystem` (fill draw list).
  *Commit: `[Phase4/Slice2] scene and system scheduler`*

- [ ] **4.3 — Camera system**
  `CameraSystem`: project + view matrix from active `Camera` + `Transform`.
  Push to GPU via per-frame uniform buffer. Update GBuffer + lighting shaders to use it.
  *Commit: `[Phase4/Slice3] camera system and per-frame uniforms`*

- [ ] **4.4 — glTF scene graph loading**
  `GltfLoader` now creates entities: nodes → `Transform` + `MeshRenderer`, scene hierarchy → `Parent`/`Children`.
  Load a multi-node glTF scene and render it correctly.
  *Commit: `[Phase4/Slice4] glTF scene graph to ECS`*

---

## Phase 5 — Skeletal Animation

Goal: a glTF character with skin and animations plays back and renders correctly.

- [ ] **5.1 — Skeleton asset loading**
  `src/animation/skeleton.cpp`: load glTF `skin` — joint names, parent indices, inverse bind matrices.
  Store as `SkeletonAsset`. Add `SkinnedMesh` component.
  *Commit: `[Phase5/Slice1] skeleton asset loading`*

- [ ] **5.2 — Animation clip loading**
  `src/animation/clip.cpp`: load glTF `animation` channels (translation/rotation/scale samplers).
  Store as `AnimClipAsset`. Support LINEAR and STEP interpolation. CUBICSPLINE deferred.
  *Commit: `[Phase5/Slice2] animation clip loading`*

- [ ] **5.3 — Animator + local-space sampling**
  `src/animation/animator.cpp`: `Animator` component. `AnimationSystem` samples clip at current time,
  writes local TRS per joint, `compute_world_transforms()` walks hierarchy.
  Produces `joint_matrices` array (skinning matrix = joint_world × inverse_bind).
  *Commit: `[Phase5/Slice3] animator and joint matrix computation`*

- [ ] **5.4 — GPU skinning compute pass**
  `assets/shaders/skinning.slang` + `src/renderer/skinning_pass.cpp`:
  Compute shader reads original vertex buffer + joint matrices SSBO, writes skinned positions + normals.
  Runs before GBuffer pass. GBuffer pass reads skinned buffer.
  *Commit: `[Phase5/Slice4] GPU skinning compute pass`*

- [ ] **5.5 — Animation blending**
  `src/animation/animator.cpp`: linear 1D blend between two clips (blend weight parameter).
  Additive layer support (per-joint mask bitmask). `BlendTree` struct.
  *Commit: `[Phase5/Slice5] animation blending and additive layers`*

- [ ] **5.6 — Animation state machine**
  `AnimStateMachine`: states, parameterized transitions, crossfade (linear blend during transition duration).
  *Commit: `[Phase5/Slice6] animation state machine with crossfade`*

---

## Phase 6 — Physics

Goal: rigid bodies simulate, a character controller walks on a static mesh collider.

- [ ] **6.1 — Jolt integration**
  `src/physics/physics_world.cpp`: `JPH::PhysicsSystem`, broad/narrow phase, fixed timestep
  accumulator (1/120s), Jolt job system wired to engine thread pool.
  Collision layers: STATIC, DYNAMIC, CHARACTER, TRIGGER.
  *Commit: `[Phase6/Slice1] Jolt Physics world integration`*

- [ ] **6.2 — Collider shapes + RigidBody component**
  `src/physics/collider.cpp`: shape factory helpers (box, sphere, capsule, convex hull, mesh).
  `PhysicsSystem`: on entity creation with `RigidBody`+`Collider`, create Jolt body.
  On tick: sync kinematic transforms Jolt→ECS and ECS→Jolt for dynamic bodies.
  *Commit: `[Phase6/Slice2] collider shapes and rigid body sync`*

- [ ] **6.3 — Character controller**
  `src/physics/character.cpp`: `JPH::CharacterVirtual` wrapper.
  `CharacterController` component: move input → velocity, `UpdateCharacter` system.
  Gravity, ground detection, step-up.
  *Commit: `[Phase6/Slice3] character controller`*

- [ ] **6.4 — Trigger / contact callbacks**
  Implement Jolt contact listener. `TriggerEvent` posted to a per-frame event queue.
  Systems can iterate trigger events this frame.
  *Commit: `[Phase6/Slice4] trigger and contact callbacks`*

---

## Phase 7 — Shadows & Lighting Improvements

Goal: directional shadow maps, point lights, basic sky.

- [ ] **7.1 — Cascaded shadow maps**
  `src/renderer/shadow_pass.cpp` + `assets/shaders/shadow.slang`:
  4-cascade CSM for the directional light. Depth-only pass per cascade.
  Sample in lighting pass with PCF.
  *Commit: `[Phase7/Slice1] cascaded shadow maps`*

- [ ] **7.2 — Point lights**
  `PointLight` component. Clustered light list built on CPU, uploaded as SSBO.
  Lighting compute shader samples cluster for the fragment's tile.
  *Commit: `[Phase7/Slice2] clustered point lights`*

- [ ] **7.3 — Procedural sky**
  `assets/shaders/sky.slang`: Preetham/Hillaire analytic sky model.
  Sky pass renders to HDR buffer before transparent pass.
  *Commit: `[Phase7/Slice3] procedural sky`*

---

## Phase 8 — Post-Processing

Goal: TAA, bloom, exposure, final image looks polished.

- [ ] **8.1 — Temporal Anti-Aliasing**
  Jitter camera per frame (Halton sequence). `assets/shaders/taa.slang`:
  reproject previous frame, history blend, neighbourhood clamp.
  *Commit: `[Phase8/Slice1] temporal anti-aliasing`*

- [ ] **8.2 — Bloom**
  Dual-kawase downsample/upsample chain. Threshold + intensity parameters.
  Composite onto HDR buffer before tonemap.
  *Commit: `[Phase8/Slice2] bloom`*

- [ ] **8.3 — Exposure & auto-exposure**
  Luminance histogram compute pass → exposure value. Lerp toward target EV over time.
  *Commit: `[Phase8/Slice3] exposure and auto-exposure`*

---

## Phase 9 — Editor (ImGui)

Goal: a usable in-engine editor with scene hierarchy, inspector, asset browser, and viewport.

- [ ] **9.1 — ImGui integration**
  `src/renderer/imgui_pass.cpp`: ImGui Vulkan + SDL3 backend. Docking layout.
  Render as final overlay pass. Gated behind `ENGINE_EDITOR` CMake flag.
  *Commit: `[Phase9/Slice1] ImGui integration with docking`*

- [ ] **9.2 — Viewport panel**
  Scene renders to offscreen `GpuImage`. Displayed in ImGui viewport panel via `ImGui::Image`.
  Viewport camera controlled with mouse (orbit / fly) when panel is focused.
  *Commit: `[Phase9/Slice2] editor viewport panel`*

- [ ] **9.3 — Scene hierarchy panel**
  `src/editor/scene_hierarchy.cpp`: tree view of entities (using `Name` + `Parent`/`Children`).
  Click to select. Right-click: create entity, delete entity, duplicate.
  *Commit: `[Phase9/Slice3] scene hierarchy panel`*

- [ ] **9.4 — Inspector panel**
  `src/editor/inspector.cpp`: for selected entity, reflect all attached components via
  `ComponentInspector<T>` template specialisations. Edit Transform, MeshRenderer, lights inline.
  *Commit: `[Phase9/Slice4] inspector panel`*

- [ ] **9.5 — Asset browser**
  `src/editor/asset_browser.cpp`: filesystem tree under `assets/`. Icon thumbnails.
  Drag mesh/texture/material onto viewport or inspector slot.
  *Commit: `[Phase9/Slice5] asset browser`*

- [ ] **9.6 — Animation preview**
  `src/editor/animation_preview.cpp`: timeline scrubber, play/pause/loop, blend weight sliders.
  Joint hierarchy overlay on viewport (lines via ImGui draw list).
  *Commit: `[Phase9/Slice6] animation preview panel`*

- [ ] **9.7 — Physics debug draw**
  Implement Jolt `DebugRenderer` interface → emit lines into ImGui draw list.
  Toggle per collision layer. Show contact points.
  *Commit: `[Phase9/Slice7] physics debug draw`*

- [ ] **9.8 — ImGuizmo gizmos**
  Translate/rotate/scale gizmos for selected entity's `Transform` in the viewport.
  Mode toggle in toolbar (W/E/R shortcuts).
  *Commit: `[Phase9/Slice8] ImGuizmo transform gizmos`*

---

## Phase 10 — Async Asset Loading & Hot Reload

Goal: assets stream in without hitches; shader edits reload live.

- [ ] **10.1 — Async asset loading**
  `AssetManager` dispatches loads to the thread pool. `AssetHandle<T>` has `is_ready()`.
  Renderer shows fallback (default white mesh, 1×1 white texture) while loading.
  *Commit: `[Phase10/Slice1] async asset loading`*

- [ ] **10.2 — Shader hot-reload**
  File watcher (inotify on Linux, ReadDirectoryChangesW on Windows) detects `.slang` changes.
  Triggers `slangc` recompile, reloads SPIR-V, recreates affected pipelines on next frame.
  *Commit: `[Phase10/Slice2] shader hot-reload`*

- [ ] **10.3 — Asset hot-reload**
  File watcher on `assets/` triggers re-cook + re-upload for changed meshes and textures.
  *Commit: `[Phase10/Slice3] asset hot-reload`*

---

## Phase 11 — Hardening & Stretch Goals

These are tackled in any order once Phase 10 is done.

- [ ] Mesh shaders (`VK_EXT_mesh_shader`) for meshlet culling
- [ ] Ray-traced shadows / ambient occlusion (`VK_KHR_ray_tracing_pipeline`)
- [ ] FABRIK IK solver
- [ ] CUBICSPLINE animation interpolation
- [ ] Lua scripting via sol2
- [ ] Audio via miniaudio
- [ ] GPU-driven indirect rendering (one `vkCmdDrawIndexedIndirect` per pass)
- [ ] Frustum + occlusion culling on GPU (Hi-Z)
- [ ] Serialize/deserialize scene to JSON or binary flatbuffer
