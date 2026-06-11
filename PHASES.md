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

- [x] **3.2 — glTF static mesh loading**
  `src/scene/gltf_loader.cpp` using fastgltf: load positions, normals, UVs, tangents, indices.
  Upload to device-local vertex + index buffers. Store as `MeshAsset`.
  *Commit: `[Phase3/Slice2] glTF static mesh loading`*

- [x] **3.3 — Texture loading**
  Load glTF images (base colour, normal, metallic-roughness, emissive) via fastgltf.
  Upload to `GpuImage`, create `VkImageView` + `VkSampler`. Store as `TextureAsset`.
  *Commit: `[Phase3/Slice3] glTF texture loading`*

- [x] **3.4 — Material asset + Slang material interface**
  Define `IMaterial` Slang interface. Implement `PbrMaterial` struct.
  `MaterialAsset` stores GPU buffer with material parameters + texture descriptors.
  *Commit: `[Phase3/Slice4] material asset and Slang material interface`*

- [x] **3.5 — GBuffer pass**
  `assets/shaders/gbuffer.slang` + `src/renderer/mesh_pass.cpp`:
  Deferred GBuffer: albedo (RGB), normal (RG oct-encoded), material (metallic/roughness/AO), depth.
  Render all `MeshRenderer` entities.
  *Commit: `[Phase3/Slice5] deferred GBuffer pass`*

- [x] **3.6 — Lighting pass**
  `assets/shaders/lighting.slang` + `src/renderer/lighting_pass.cpp`:
  Fullscreen compute pass reading GBuffer, applying one directional light, writing HDR colour.
  *Commit: `[Phase3/Slice6] deferred lighting pass`*

- [x] **3.7 — Tonemap + present**
  `assets/shaders/tonemap.slang`: ACES tonemap, gamma correction, write to swapchain image.
  Static glTF mesh now visible with correct lighting.
  *Commit: `[Phase3/Slice7] tonemap pass and lit static mesh`*

---

## Phase 4 — ECS & Scene

Goal: entities with components drive rendering; a scene can be constructed in code.

- [x] **4.1 — ECS world + core components**
  Integrate entt. Define all core components from `components.hpp` (Transform, MeshRenderer,
  Camera, DirectionalLight, Name, Parent, Children).
  *Commit: `[Phase4/Slice1] ECS world and core components`*

- [x] **4.2 — Scene + system scheduler**
  `src/scene/scene.cpp`: owns `entt::registry`, registers and ticks systems in fixed order.
  Systems: `TransformSystem` (propagate world matrices), `RenderCollectSystem` (fill draw list).
  *Commit: `[Phase4/Slice2] scene and system scheduler`*

- [x] **4.3 — Camera system**
  `CameraSystem`: project + view matrix from active `Camera` + `Transform`.
  Push to GPU via per-frame uniform buffer. Update GBuffer + lighting shaders to use it.
  *Commit: `[Phase4/Slice3] camera system and per-frame uniforms`*

- [x] **4.4 — glTF scene graph loading**
  `GltfLoader` now creates entities: nodes → `Transform` + `MeshRenderer`, scene hierarchy → `Parent`/`Children`.
  Load a multi-node glTF scene and render it correctly.
  *Commit: `[Phase4/Slice4] glTF scene graph to ECS`*

- [x] **4.5 — PBR direct lighting**
  Upgrade the deferred lighting compute pass from Lambert to a Cook-Torrance microfacet
  BRDF (GGX NDF, height-correlated Smith visibility, Schlick Fresnel) for the directional
  light. Reconstruct world position from the GBuffer depth + inverse view-proj so the pass
  has the per-pixel view vector; feed it the camera position. Makes the loaded
  metallic/roughness data actually drive shading. IBL/environment ambient deferred to Phase 7/8.
  *Commit: `[Phase4/Slice5] PBR direct lighting`*

---

## Phase 5 — Skeletal Animation

Goal: a glTF character with skin and animations plays back and renders correctly.

- [x] **5.1 — Skeleton asset loading**
  `src/animation/skeleton.cpp`: load glTF `skin` — joint names, parent indices, inverse bind matrices.
  Store as `SkeletonAsset`. Add `SkinnedMesh` component.
  *Commit: `[Phase5/Slice1] skeleton asset loading`*

- [x] **5.2 — Animation clip loading**
  `src/animation/clip.cpp`: load glTF `animation` channels (translation/rotation/scale samplers).
  Store as `AnimClipAsset`. Support LINEAR and STEP interpolation. CUBICSPLINE deferred.
  *Commit: `[Phase5/Slice2] animation clip loading`*

- [x] **5.3 — Animator + local-space sampling**
  `src/animation/animator.cpp`: `Animator` component. `AnimationSystem` samples clip at current time,
  writes local TRS per joint, `compute_world_transforms()` walks hierarchy.
  Produces `joint_matrices` array (skinning matrix = joint_world × inverse_bind).
  *Commit: `[Phase5/Slice3] animator and joint matrix computation`*

- [x] **5.4 — GPU skinning compute pass**
  `assets/shaders/skinning.slang` + `src/renderer/skinning_pass.cpp`:
  Compute shader reads original vertex buffer + joint matrices SSBO, writes skinned positions + normals.
  Runs before GBuffer pass. GBuffer pass reads skinned buffer.
  *Commit: `[Phase5/Slice4] GPU skinning compute pass`*

- [x] **5.5 — Animation blending**
  `src/animation/animator.cpp`: linear 1D blend between two clips (blend weight parameter).
  Additive layer support (per-joint mask bitmask). `BlendTree` struct.
  *Commit: `[Phase5/Slice5] animation blending and additive layers`*

- [x] **5.6 — Animation state machine**
  `AnimStateMachine`: states, parameterized transitions, crossfade (linear blend during transition duration).
  *Commit: `[Phase5/Slice6] animation state machine with crossfade`*

---

## Phase 6 — Physics

Goal: rigid bodies simulate, a character controller walks on a static mesh collider.

- [x] **6.1 — Jolt integration**
  `src/physics/physics_world.cpp`: `JPH::PhysicsSystem`, broad/narrow phase, fixed timestep
  accumulator (1/120s), Jolt job system wired to engine thread pool.
  Collision layers: STATIC, DYNAMIC, CHARACTER, TRIGGER.
  *Commit: `[Phase6/Slice1] Jolt Physics world integration`*

- [x] **6.2 — Collider shapes + RigidBody component**
  `src/physics/collider.cpp`: shape factory helpers (box, sphere, capsule, convex hull, mesh).
  `PhysicsSystem`: on entity creation with `RigidBody`+`Collider`, create Jolt body.
  On tick: sync kinematic transforms Jolt→ECS and ECS→Jolt for dynamic bodies.
  *Commit: `[Phase6/Slice2] collider shapes and rigid body sync`*

- [x] **6.3 — Character controller**
  `src/physics/character.cpp`: `JPH::CharacterVirtual` wrapper.
  `CharacterController` component: move input → velocity, `UpdateCharacter` system.
  Gravity, ground detection, step-up.
  *Commit: `[Phase6/Slice3] character controller`*

- [x] **6.4 — Trigger / contact callbacks**
  Implement Jolt contact listener. `TriggerEvent` posted to a per-frame event queue.
  Systems can iterate trigger events this frame.
  *Commit: `[Phase6/Slice4] trigger and contact callbacks`*

---

## Phase 7 — Shadows & Lighting Improvements

Goal: directional shadow maps, point lights, basic sky, transparent & glass materials.

- [x] **7.1 — Cascaded shadow maps**
  `src/renderer/shadow_pass.cpp` + `assets/shaders/shadow.slang`:
  4-cascade CSM for the directional light. Depth-only pass per cascade.
  Sample in lighting pass with PCF.
  *Commit: `[Phase7/Slice1] cascaded shadow maps`*

- [x] **7.2 — Point lights**
  `PointLight` component. Clustered light list built on CPU, uploaded as SSBO.
  Lighting compute shader samples cluster for the fragment's tile.
  *Commit: `[Phase7/Slice2] clustered point lights`*

- [x] **7.3 — Procedural sky**
  `assets/shaders/sky.slang`: Preetham/Hillaire analytic sky model.
  Sky pass renders to HDR buffer before transparent pass.
  *Commit: `[Phase7/Slice3] procedural sky`*

- [x] **7.4 — Forward transparent pass**
  Split scene collection into opaque vs alpha-blended draws. New forward `TransparentPass`
  after the deferred lighting that writes into the HDR buffer, depth-tested against the opaque
  GBuffer depth, reusing the PBR BRDF (factored out of `lighting.slang` into a shared Slang
  module). Back-to-front sorted. Alpha-blend (`BLEND`) and alpha-cutout (`MASK`) materials from
  glTF render correctly.
  *Commit: `[Phase7/Slice4] forward transparent pass`*

- [x] **7.5 — Glass / transmission (KHR_materials_transmission)**
  Load `KHR_materials_transmission` / `_volume` / `_ior` in the glTF loader. Copy the opaque
  HDR result into a mip-chained scene-colour texture; transmissive surfaces in the transparent
  pass sample it at a refracted screen-space offset (roughness selects the mip, for frosted vs
  clear glass), Fresnel-mix reflection vs refraction, and apply Beer-Lambert volume attenuation
  from thickness. Renders the glass dragon (`DragonAttenuation`) correctly.
  *Commit: `[Phase7/Slice5] glass / transmission`*

- [x] **7.6 — Image-Based Lighting (split-sum IBL)**
  Industry-standard environment lighting (the same pipeline the glTF Sample Viewer uses), so
  metals and smooth dielectrics reflect the environment instead of reading as flat grey. A
  one-time precompute (rebuilt only when the environment changes): bake the environment into a
  cubemap (from the procedural sky now, an equirectangular `.hdr` later), convolve a diffuse
  **irradiance** cubemap (cosine-weighted), generate a **prefiltered specular** cubemap mip chain
  (GGX importance sampling, roughness → mip), and integrate the **BRDF LUT** (split-sum
  scale/bias). The deferred lighting pass adds the IBL ambient term: `kD·irradiance·albedo +
  prefiltered·(F·brdf.x + brdf.y)`, scaled by AO. Replaces the flat constant ambient. Needs
  cubemap RHI support (cube-compatible images, cube + per-face/per-mip array views). Verified by
  a self-test (LUT/irradiance/prefilter sanity + a metal surface that brightens and takes on the
  environment) and visually on DamagedHelmet (reflective visor + chrome).
  *Commit: `[Phase7/Slice6] image-based lighting`*

- [x] **7.7 — Physically-based transmission overhaul**
  Rework 7.5 to match the glTF spec / sample-viewer reference instead of approximating
  transmission with alpha blending. Transmissive surfaces get their own pipeline: depth-write
  on, opaque blend state, per-draw culling via `VK_DYNAMIC_STATE_CULL_MODE` (honouring glTF
  `doubleSided`, parsed in the loader) — eliminating the self-overdraw speckles. The shader
  composites the background itself: view ray refracted by the real IOR (`KHR_materials_ior`,
  dielectric f0 derived from it), walked through the volume thickness (`thicknessFactor` ×
  `thicknessTexture.g`, node-scale aware) and the exit point projected back to screen space;
  the opaque scene colour is sampled from a blit-downsampled mip chain (LOD from roughness ×
  IOR, per sample viewer) and tinted by Beer-Lambert attenuation (`KHR_materials_volume`
  attenuation colour/distance). Glass also receives split-sum IBL reflection + sun specular,
  and transmissive meshes now cast shadows. Swapchain gains TRANSFER_SRC + a `QUAT_SCREENSHOT`
  PPM dump for headless visual verification. Verified on `DragonAttenuation` (amber dragon
  matches reference) and the showcase scene.
  *Commit: `[Phase7/Slice7] physically-based transmission: volume, IOR, rough refraction, glass shadows`*

- [x] **7.8 — Texture mipmaps, KHR_texture_transform, emissive strength**
  Production texture pipeline fixes driven by the CarConcept sample. glTF textures now upload
  a full mip chain (vkCmdBlitImage downsample at upload; upload contexts moved to the graphics
  queue, where blits are legal) and sample with trilinear + anisotropic filtering
  (`samplerAnisotropy` enabled when the device offers it). `KHR_texture_transform` is parsed
  per texture slot (offset/rotation/scale folded into a 2x2 matrix + offset in the material
  params, applied in the GBuffer and transparent shaders) — CarConcept's car-paint flake
  normal map tiles 30x30 and its interior fabrics 200x400 instead of stretching once across
  the mesh. `KHR_materials_emissive_strength` premultiplies the emissive factor. Normal
  sampling reconstructs Z from XY (robust against degenerate/mip-averaged texels). Also fixes
  a latent std140 mismatch: Slang `float3` padding aligns to 16 bytes, so the shader-side
  material struct silently diverged from the C++ layout past offset 84 — padding is now three
  scalar floats and the struct is 240 bytes on both sides.
  *Commit: `[Phase7/Slice8] texture mipmaps + anisotropy, KHR_texture_transform, emissive strength`*

- [x] **7.9 — Clearcoat + geometric specular anti-aliasing**
  `KHR_materials_clearcoat` (factors; coat textures deferred): clearcoat factor/roughness ride
  in the former material-params padding, the GBuffer gains a fourth RG8 target (coat strength +
  roughness), and the deferred lighting layers a second dielectric specular lobe (f0 = 0.04)
  per the glTF spec — `coated = base·(1 − cc·Fc) + cc·f_coat` — for the sun, point lights, and
  IBL (prefiltered reflection at the coat roughness). Geometric specular AA (Kaplanyan /
  Filament `normalFiltering`): at GBuffer-write time the screen-space variance of the shading
  normal widens the stored perceptual roughness (clamped kernel), and the forward transparent
  shader applies the same — sub-pixel normal detail reads as roughness instead of sparkle.
  Kills CarConcept's interior specular-sparkle grid and trim dots; the paint gains the authored
  coat sheen. New self-tests: clearcoat (coat adds an achromatic head-on highlight a rough
  diffuse base cannot produce) and specular AA (flat normals stay at authored roughness 0,
  divergent normals widen), plus clearcoat/texture-transform/emissive-strength coverage in the
  material extract test.
  *Commit: `[Phase7/Slice9] clearcoat + geometric specular anti-aliasing`*

---

## Phase 8 — Post-Processing

Goal: TAA, bloom, exposure, final image looks polished.

- [x] **8.1 — Temporal Anti-Aliasing**
  Jitter camera per frame (Halton sequence). `assets/shaders/taa.slang`:
  reproject previous frame, history blend, neighbourhood clamp.
  *Commit: `[Phase8/Slice1] temporal anti-aliasing`*

- [x] **8.2 — Bloom**
  Dual-kawase downsample/upsample chain. Threshold + intensity parameters.
  Composite onto HDR buffer before tonemap.
  *Commit: `[Phase8/Slice2] bloom`*

- [x] **8.3 — Exposure & auto-exposure**
  Luminance histogram compute pass → exposure value. Lerp toward target EV over time.
  *Commit: `[Phase8/Slice3] exposure and auto-exposure`*

---

## Phase 9 — Editor (ImGui)

Goal: a usable in-engine editor with scene hierarchy, inspector, asset browser, and viewport.

- [x] **9.1 — ImGui integration**
  `src/renderer/imgui_pass.cpp`: a from-scratch ImGui renderer on the engine RHI — the stock
  `imgui_impl_vulkan` backend allocates descriptor sets from pools, which cannot be mixed with
  `VK_EXT_descriptor_buffer` in one command buffer (and is banned by CLAUDE.md). One pipeline
  (`imgui.slang`: pixel-space verts → clip via push constant, sRGB-linearised vertex colours so
  the sRGB swapchain attachment composites correctly), per-frame host-visible vertex/index
  upload, scissor per draw command, font atlas + sampler through a descriptor buffer. Runs as
  the final pass over the swapchain image (loadOp LOAD after the TAA resolve). The SDL3
  platform backend (input only) is vendored under `src/editor/vendor/` — the vcpkg
  `imgui[sdl3-binding]` feature re-enables SDL3's default features (dbus/ibus → libsystemd), a
  dependency chain we don't want. `EditorLayer` (`src/editor/editor.cpp`) owns the ImGui
  context: docking enabled, fullscreen passthrough dockspace, main menu bar, Stats panel, demo
  window toggle; `wants_mouse()/wants_keyboard()` gate game input. `QUAT_NO_UI=1` disables the
  editor for deterministic headless renders. All gated behind `ENGINE_EDITOR`.
  *Commit: `[Phase9/Slice1] ImGui integration with docking`*

- [x] **9.2 — Viewport panel**
  The frame resolves into a per-frame-slot offscreen `GpuImage` (swapchain format, TAA copy
  target) instead of the swapchain when the editor is active; the Viewport panel samples it via
  `ImGui::Image` (ImGuiPass texture registry: font id 1, viewport id 2, descriptor rebound per
  frame so panel resizes track the recreated image). The scene renders at the panel's content
  size (camera aspect follows), the UI pass clears + owns the whole swapchain, and DockBuilder
  lays out the initial dock split (Viewport centre, Hierarchy/Stats left, Inspector/Renderer
  right, Assets/Animation bottom — future panels docked by name). Free-fly camera input only
  engages over the Viewport panel. `QUAT_NO_UI` keeps the direct-to-swapchain path.
  *Commit: `[Phase9/Slice2] editor viewport panel`*

- [x] **9.3 — Scene hierarchy panel**
  `src/editor/scene_hierarchy.cpp`: tree view of entities (using `Name` + `Parent`/`Children`),
  roots = live entities without a parent. Click to select (selection lives in EditorLayer, not
  the ECS). Right-click an item: create child, duplicate (shallow component clone), delete
  (recursive subtree destroy, unlinked from the parent first); right-click blank space: create
  entity. Mutation-safe iteration via snapshots.
  *Commit: `[Phase9/Slice3] scene hierarchy panel`*

- [x] **9.4 — Inspector panel**
  `src/editor/inspector.cpp`: for the selected entity, component editors via
  `ComponentInspector<T>` specialisations — Name, Transform (decompose → T/euler/S → recompose),
  DirectionalLight (the live sun control, since lighting reads the ECS), PointLight, Camera,
  MeshRenderer (info), Animator. Plus the Renderer panel: bloom parameter sliders feeding the
  frame's BloomParams through `editor::RendererSettings`, and an IBL rebake button consumed at
  frame start.
  *Commit: `[Phase9/Slice4] inspector + renderer settings panel`*

- [x] **9.5 — Asset browser**
  `src/editor/asset_browser.cpp`: filesystem tree of `assets/` + `sample-assets/` (project root
  from a compile definition), directories-first sorted, type tags per extension (real thumbnails
  deferred). glTF/GLB files instantiate into the scene on double-click or by dragging onto the
  Viewport panel (ImGui drag-and-drop payload); the frame loop consumes the request through a
  transient graphics-queue upload context, like the startup path.
  *Commit: `[Phase9/Slice5] asset browser`*

- [x] **9.6 — Animation preview**
  `src/editor/animation_preview.cpp`: timeline scrubber (0..clip duration), play/pause (resume
  speed remembered in editor state), loop + speed controls for the selected entity's Animator.
  Joint hierarchy overlay over the viewport: joint world positions recovered by applying each
  skinning matrix to its joint's bind-pose world position, projected with the scene camera and
  drawn as bone lines + joint dots via the ImGui foreground draw list (clipped to the panel).
  Blend-weight sliders arrive with a BlendTree component (the ECS Animator plays single clips).
  *Commit: `[Phase9/Slice6] animation preview panel`*

- [x] **9.7 — Physics debug draw**
  `src/editor/physics_debug.cpp`: collision-shape wireframes (box edges, sphere great circles,
  capsule caps + side lines) drawn from the ECS Collider components, projected over the
  Viewport via the ImGui foreground draw list, colour-coded and toggleable per category
  (static green / dynamic+kinematic blue / sensors yellow). vcpkg's Jolt is compiled without
  `JPH_DEBUG_RENDERER`, so the engine renders its own shape descriptors instead of implementing
  the Jolt interface against a library that cannot service it; contact points are deferred to a
  Jolt build with the debug renderer enabled.
  *Commit: `[Phase9/Slice7] physics debug draw`*

- [x] **9.8 — ImGuizmo gizmos, picking, theme**
  ImGuizmo (vcpkg) translate/rotate/scale on the selected entity: manipulates the WORLD matrix
  (unflipped projection — ImGuizmo expects y-up NDC), converted back to local through the
  parent's world. W/E/R switch modes over the viewport (except during right-drag, which owns
  WASD for the fly camera — fly movement now requires holding RMB, Unity-style) + a floating
  mode toolbar in the panel corner. Left-click picking: ray through the clicked pixel
  (flipped view-proj, same convention as the renderer) against each mesh's local AABB
  (slab test in mesh space), nearest hit selects. Polish: vendored Roboto-Medium UI font and a
  custom dark theme (flat panels, accent blue, consistent rounding).
  *Commit: `[Phase9/Slice8] ImGuizmo gizmos, click picking, editor theme`*

- [x] **9.9 — Play mode + physics authoring + sun control**
  Unity-style Play/Stop in the menu bar: Play snapshots the whole scene (`SceneSnapshot` —
  every entity + component copied; restore rebuilds the registry with EXACT entity ids via
  entt `create(hint)`, so hierarchy links and the selection survive) and brings up a dedicated
  `PhysicsWorld`; `physics_system` runs while playing; Stop tears the world down and restores
  the snapshot. Inspector gains component authoring: "Add component" popup (Collider,
  RigidBody, lights, Camera), right-click header to remove, and full Collider (shape /
  dimensions / offset / sensor) + RigidBody (motion / mass) editors. Renderer panel gains
  UE-style sun control: azimuth/elevation sliders drive the scene's DirectionalLight live
  (sky + direct light follow the drag) and the IBL environment rebakes automatically on
  slider release. Self-test: snapshot round trip (mutate/destroy/create, verify exact-id
  restore).
  *Commit: `[Phase9/Slice9] editor play mode, collider authoring, sun control`*

- [x] **9.10 — Scene save/load**
  JSON scene serialization (`scene_io`): flat entity array with parent indices, Transform/Name/
  Collider/RigidBody/lights/Camera, and mesh references through the new `MeshSource` provenance
  component (AssetManager cache keys + originating glTF path — the glTF loader and the showcase
  tag every renderable). Loading clears the scene, re-instantiates referenced glTF files into a
  scratch scene to repopulate the asset cache, resolves handles by cache key, rebuilds the
  hierarchy, and ticks once. Editor File menu (path field + Save/Load) feeds requests the frame
  loop consumes; on load the loop rebinds its camera, clears the selection, and rebakes the
  IBL. Skinned meshes/animators are not yet serialized. Self-test: full round trip through a
  temp file (hierarchy, collider, rigid body, light, mesh re-resolution).
  *Commit: `[Phase9/Slice10] scene save/load (JSON)`*

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

---

## Phase 11 — Atmosphere & Volumetric Clouds

Goal: a fully dynamic sky — movable sun with correct scattering, volumetric clouds.

- [x] **11.1 — Physically-based sky** (Hillaire 2020): `lib/atmosphere.slang` models the
  Rayleigh/Mie/ozone medium with Bruneton's transmittance parameterisation and Hillaire's
  sky-view warp. Three LUTs (`AtmospherePass`): transmittance 256x64 + multiple-scattering
  32x32 computed once at startup, and a 192x108 sky-view LUT (32-step single-scatter march
  with the multi-scatter term + sun-lit ground for below-horizon rays) recomputed whenever
  the sun moves — so the Renderer panel's sun sliders drive a fully physical sky. The
  lighting background samples the sky-view LUT (+ analytic transmittance-tinted sun disc),
  the direct sun light is transmittance-tinted (red sunsets on geometry), and the IBL
  environment bakes from the same LUTs so ambient/reflections match. The procedural sky
  remains as a fallback (sky mode tri-state in the push constants), keeping all pre-existing
  self-tests exact. Self-test: LUT readbacks — horizon ray reddened vs zenith, sky-view
  blue-dominant above the horizon and darker below. Aerial perspective on geometry and
  time-of-day animation defer to 11.2/11.3.
  *Commit: `[Phase11/Slice1] physically-based sky (Hillaire LUTs)`*
- [x] **11.2 — Volumetric clouds**: a parameterizable cloud shell marched **per pixel** in the
  lighting background (and per texel in the IBL bake — the sky-view LUT smears anything
  high-frequency, so it stays atmosphere-only): value-noise FBM density (coverage threshold +
  detail erosion + rounded height profile), Beer-Lambert absorption, a 5-step sun shadow
  march, Henyey-Greenstein phase (+powder), zenith-sampled sky ambient, and aerial perspective
  so distant banks fade into haze instead of walling the horizon. Cloud transmittance dims the
  analytic sun disc. Interleaved-gradient-noise jitter on the march start kills step banding.
  All artistic parameters (enable, coverage, density, altitude, thickness, puff size, detail)
  live in `CloudSettings`, packed into two push-constant float4s and driven live from the
  editor's Renderer panel (IBL rebakes on slider release, like the sun). Full-parallax
  volumetrics with a real noise-texture stack + temporal reprojection (Schneider-style) are
  the follow-up.
  Cloud noise hashes are sin-free (Hoskins hash12 — `frac(sin())` collapses into blocky
  lattice cells on hardware fast-trig like NVIDIA), with octave rotation in the FBM, and the
  field drifts with an editor-controlled wind (speed + heading; the detail layer advects
  faster so shapes churn rather than translate rigidly).
  *Commits: `[Phase11/Slice2] volumetric clouds in the sky-view LUT`,
  `[Phase11/Slice2.1] clouds marched per pixel - sharp shapes`,
  `[Phase11/Slice2.2] cloud march jitter (de-banding) + editor cloud controls`,
  `[Phase11/Slice2.3] sin-free cloud hash (NVIDIA blocks) + wind drift`*
- [x] **11.3 — Dynamic IBL**: the environment probe re-renders incrementally inside the render
  graph — one compute step per frame (6 env-cube faces in 128x128 tiles to bound the per-frame
  cloud-march cost, the irradiance convolution, then one prefiltered mip per frame; 30-step
  cycle) into the back set of two ping-ponged map sets, flipping on completion. Between cycles
  the probe idles unless the sun/cloud settings changed or the cloud-drift refresh cadence
  (1 s) is due. Lighting/transparent take a non-owning `IblViewSet` fetched per frame; the
  BRDF LUT is sun-independent and baked once. The sky-view LUT recompute also moved in-graph
  (`add_skyview_update_to_graph`, dirty-checked), so sun drags and drifting clouds update
  ambient/reflections continuously with zero pipeline stalls — the slider-release rebake hack
  and the Renderer panel's rebake button are gone. A startup blocking cycle keeps frame one
  fully lit. Self-test: an incremental cycle with a near-horizon sun (atmosphere LUT path)
  flips the sets and dims the zenith irradiance vs the initial noon bake.
  *Commit: `[Phase11/Slice3] dynamic IBL - incremental in-graph probe refresh`*

## Phase 12 — Terrain

Goal: large-scale procedurally generated terrain, editable and walkable.

- [x] **12.1 — Heightmap generation**: `engine_terrain` lib — seeded gradient-noise FBM with
  iq domain warping, composed as a low-frequency continental mask blending rolling plains
  into ridged mountains, plus particle-based hydraulic erosion (Beyer droplets — gullies and
  sediment fans). Fully deterministic per seed (PCG32 everywhere, serial droplets); the FBM
  pass parallelises across rows on std::jthread; `generate_async()` runs off the main thread
  (Phase 10's pool will absorb it). `Heightmap` carries metres, bilinear `sample()` and
  central-difference `normal()` for 12.2/12.3. Self-test: bit-exact determinism, seed
  sensitivity, bounds, warp effect, erosion-smooths-gradient.
  *Commit: `[Phase12/Slice1] seeded heightmap generation (FBM + warp + erosion)`*
- [ ] **12.2 — Chunked LOD rendering**: quadtree chunks with skirt stitching (or geometry
  clipmaps), normal/splat maps, triplanar material blending.
- [x] **12.3 — Terrain physics**: `PhysicsWorld::create_height_field` (Jolt
  `HeightFieldShapeSettings`) + `scene::add_terrain_body` — a static height-field body for the
  Terrain entity, edge-padded one duplicated row/column so the 2^n+1 grid satisfies Jolt's
  block alignment while every interior cell aligns exactly with the rendered texels. Play
  mode adds the body alongside the collider-only statics (the frame loop retains the CPU
  heightmap); Stop tears it down with the world. Self-test: a sphere dropped through the ECS
  physics system rests on the sampled surface height. The editor brush moves to 12.4+ (it
  needs partial re-upload, which streaming's tile machinery provides).
  *Commit: `[Phase12/Slice3] terrain physics (Jolt height field)`*
- [x] **12.4 — Terrain streaming**: `TerrainStreamer` keeps a (2*radius+1)² window of tiles
  loaded around the camera — one generation job at a time on a worker thread (nearest-missing
  first), tiles dropped as the window moves. Tiles of one seed are **seam-free**: the
  generator samples noise in world-tile coordinates (`TerrainParams::world_tile`) and fades
  erosion to zero a brush-radius before tile borders (mass-conserving), so adjacent edges are
  bit-identical (self-test asserts it). TerrainPass became multi-tile (per-tile heightmap
  texture + descriptor, shared LOD index buffers); uploads never stall — fresh images per
  tile, replaced/unloaded tiles retire for 3 frames before destruction. Play mode creates and
  destroys a static height-field body with each tile. Terrain component gains
  streaming/stream_radius (inspector + scene file). One ready tile uploads per frame (the
  single-job streamer is the natural budget). ENTITY streaming (cells of scene content) still
  defers to Phase 10's async asset pipeline as planned; the editor sculpt brush also waits
  (needs partial texture re-upload).
  *Commit: `[Phase12/Slice4] terrain streaming (seamless seeded tiles)`*

## Phase 13 — Gameplay Framework (NPCs)

Goal: NPCs that navigate and act in the world.

- [ ] **13.1 — Navmesh**: Recast/Detour generation from static geometry, debug-draw overlay.
- [ ] **13.2 — Agents**: locomotion via CharacterVirtual, path following, avoidance.
- [ ] **13.3 — Behaviour**: behaviour trees (or utility AI) as data assets; perception stubs.

## Phase 14 — Networking

Goal: multiplayer-ready replication layer.

- [ ] **14.1 — Transport**: client/server over UDP (e.g. GameNetworkingSockets/ENet style),
  connection management, channels.
- [ ] **14.2 — Replication**: snapshot deltas of ECS components, interest management.
- [ ] **14.3 — Prediction**: client-side prediction + reconciliation for the local character.
