# Geometry pipelines, upscaling/AA and real-time neural rendering (2018–2026) — tech radar for quat-3d

Scope note for the report writer: these notes cover (1) GPU-driven rendering, visibility buffer, meshlets and mesh shaders; (2) virtualised geometry (Nanite-style) and cluster acceleration structures; (3) upscaling, AA and frame generation; (4) neural and ML real-time techniques; (5) texture and mesh compression and streaming.

Two kinds of source appear below. Items marked **[verified 2026-11]** were confirmed by a web search or fetch in this session. Items marked **[foundational]** cite canonical primary sources (talk slides, papers, repositories) from prior knowledge. Their URLs are the well-known canonical locations, but they were not re-fetched here. The fetch tool could not reach zeux.io, jms55.github.io or gpuopen.com because the hosts are outside this session's network allowlist. Facts from those sites come only from search-result snippets and are marked as such.

quat-3d baseline, from the assignment: Vulkan 1.3 with dynamic rendering, descriptor buffer, sync2 and BDA. Shaders are Slang. A render graph runs compute passes. The renderer is a deferred GBuffer with GPU clustered lights. Instanced draws use BDA, are CPU-batched and are frustum-culled on the CPU. GPU culling is planned. TAA exists without upscaling. Shader hot reload works. macOS runs through MoltenVK or KosmicKrisp, and a Metal backend is planned.

---

## Q1. GPU-driven rendering: indirect multi-draw, bindless, GPU frustum + two-pass Hi-Z occlusion, visibility buffer, meshlets and mesh/task shaders with fallbacks

### Takeaway
The industry-standard first step is a compute-driven pipeline: GPU instance culling, cluster culling, two-pass Hi-Z occlusion culling, and `vkCmdDrawIndexedIndirectCount` with bindless materials. It needs only core Vulkan 1.2/1.3 features and works on MoltenVK. quat-3d already has BDA and descriptor buffers, so this is the highest-value medium-sized step. Mesh shaders (VK_EXT_mesh_shader) are a desktop-only accelerator. Neither MoltenVK nor KosmicKrisp is confirmed to expose them, so a compute + indirect-draw fallback over the same meshlet data is mandatory. A visibility buffer is the natural follow-on once cluster rendering exists.

### Cited Findings
**Foundations (GPU-driven pipelines)**
- Haar & Aaltonen, "GPU-Driven Rendering Pipelines" (SIGGRAPH 2015 Advances; Ubisoft *Assassin's Creed Unity* and RedLynx *Trials*). The talk covers mesh "clusters/chunks" of 64 triangles, GPU instance and cluster culling, and occlusion culling against a depth pyramid. It reprojects the previous frame's depth and re-tests false negatives. It also shows virtual texturing, which enables a near-single-draw-call design. [foundational] — [Advances 2015 slides](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)
- Wihlidal, "Optimizing the Graphics Pipeline with Compute" (GDC 2016; Frostbite). A compute pass culls triangles per cluster and per triangle (back-face, zero-area, small-primitive or no-sample-coverage, frustum, depth) before the fixed-function front-end. It writes compacted index buffers and drives multi-draw-indirect. It is aimed at GCN-era front-end bottlenecks. [foundational] — [GDC Vault](https://www.gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With)
- Arseny Kapoulkine's **niagara** is an open-source Vulkan renderer developed on stream. It implements meshlets, task/mesh shaders with a traditional-pipeline fallback, GPU frustum and occlusion culling, two-phase occlusion culling against a depth pyramid, and `DrawIndexedIndirectCount`. It is the most directly reusable open reference for a Vulkan 1.3 engine. [foundational] — [github.com/zeux/niagara](https://github.com/zeux/niagara)
- Two-pass ("two-phase") Hi-Z occlusion works as follows. Pass 1 draws objects that were visible last frame. It then builds a Hi-Z pyramid from that depth (min/max reduction compute). Pass 2 tests all remaining objects against the new pyramid and draws the newly visible ones. This removes the one-frame-lag popping of pure reprojection. Karis's Nanite talk uses the same scheme for clusters. [foundational] — [Karis, Nanite Advances 2021](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf); [niagara](https://github.com/zeux/niagara)

**Visibility buffer**
- Burns & Hunt, "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading" (JCGT 2013). It rasterises only a triangle/instance ID (and depth) per pixel. Attributes are re-fetched and interpolated in a later shading pass, which decouples shading from geometry density and avoids fat GBuffer bandwidth. [foundational] — [JCGT 2(2)](https://jcgt.org/published/0002/02/04/)
- Wolfgang Engel / ConfettiFX "Triangle Visibility Buffer" (2016–2018) is implemented in **The Forge** cross-platform framework (Vulkan, D3D12, Metal). It combines GPU triangle-filtering compute (Wihlidal-style) with a visibility buffer. [foundational] — [The Forge GitHub](https://github.com/ConfettiFX/The-Forge); [Engel blog, "Triangle Visibility Buffer"](http://diaryofagraphicsprogrammer.blogspot.com/2018/03/triangle-visibility-buffer.html)
- Nanite writes a 64-bit visibility buffer (depth in the high bits, cluster+triangle ID in the low bits) using 64-bit atomic max. It shades through per-material screen-space tile classification ("material depth" / shading bins in later UE versions). [foundational] — [Karis 2021](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)
- Bevy's virtual geometry moved its visibility buffer to an **R64Uint storage texture with texture atomics** in 0.16. The author reports about 0.42 ms saved in a simple scene. [verified 2026-11, search snippet] — [jms55, Virtual Geometry in Bevy 0.16](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16)

**Mesh shaders**
- VK_EXT_mesh_shader is a ratified, cross-vendor Khronos extension (task + mesh stages). It was designed for functional compatibility with D3D12 mesh shaders. [verified 2026-11] — [Vulkan docs proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html); [Khronos registry](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_mesh_shader.html)
- Khronos hosts a "Mesh Shader Culling" sample in Vulkan-Samples. [verified 2026-11] — [Vulkan Samples](https://docs.vulkan.org/samples/latest/samples/extensions/mesh_shader_culling/README.html)
- **KosmicKrisp** is LunarG's Mesa-based Vulkan-on-Metal driver for Apple Silicon. Its XDC 2025 deck says it passes the Vulkan 1.3 CTS and lists **mesh shaders as a future goal**, together with sparse memory and tessellation/geometry shaders. [verified 2026-11] — [LunarG XDC 2025 slides](https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf)
- No source found confirming VK_EXT_mesh_shader in a MoltenVK release. A `mesh-shader` branch exists in a mirror of the MoltenVK repo, but its contents are unverified. [verified 2026-11, negative result] — [MoltenVK mesh-shader branch mirror](https://skia.googlesource.com/external/github.com/KhronosGroup/MoltenVK/+/refs/heads/mesh-shader/)
- meshoptimizer provides the meshlet builder (`meshopt_buildMeshlets`) and per-meshlet culling data (`meshopt_computeMeshletBounds`, which gives a bounding sphere and normal cone). It is MIT-licensed and has no runtime dependencies. [foundational] — [github.com/zeux/meshoptimizer](https://github.com/zeux/meshoptimizer)

### Inferences
- **Fit to quat-3d.** BDA instance data plus descriptor-buffer bindless means the remaining work is mostly compute passes in the render graph. Planned passes: (a) instance frustum + Hi-Z culling producing compacted draw commands; (b) Hi-Z pyramid build (single-pass min-reduction, e.g. an SPD-style downsampler); (c) `vkCmdDrawIndexedIndirectCount`, which is core in Vulkan 1.2. All of this works on MoltenVK. This is the natural "GPU culling planned" item and should come before any mesh-shader work. **Priority: medium, start now.**
- **Meshlets without mesh shaders.** Store geometry as meshlets (64 verts / 124 tris is the common NV/meshoptimizer default). Cull meshlets in compute. Either emit a compacted index buffer (Wihlidal) or draw one indirect instance per surviving meshlet. Then add a VK_EXT_mesh_shader fast path behind a feature check (CLAUDE.md already mandates the guard). One data format then serves both paths, and Metal can later use native Metal mesh shaders.
- **Visibility buffer vs current GBuffer.** A visibility buffer pays off at high triangle density, many materials, or when it is paired with software rasterisation. It also lets the existing clustered-light compute pass shade from a thin buffer. quat-3d's GBuffer works today, so treat the visibility buffer as a **medium/long-term** step tied to virtual geometry rather than a quick win. Slang interfaces map well to material-bin shading dispatches.
- Two-pass occlusion needs a depth prepass or an early depth phase. Plan the render graph so pass 1 renders visible-last-frame geometry, the Hi-Z build is a compute node, and pass 2 runs before the lighting compute.

### Gaps
- Could not verify Activision's geometry rendering pipeline talk (title, year, URL). Searching research.activision.com is recommended. Not cited.
- Could not confirm whether any MoltenVK release exposes VK_EXT_mesh_shader as of late 2026. Check the MoltenVK `Docs/MoltenVK_Runtime_UserGuide.md` feature list.
- Tomasz Stachowiak's visibility-buffer write-ups (kajiya, Embark) were not located in this session.

---

## Q2. Virtualised geometry: Nanite, cluster DAG LOD, software rasterisation, streaming; open implementations; RTX Mega Geometry and cluster acceleration structures

### Takeaway
Nanite-style rendering (a cluster DAG with locked group boundaries, a view-dependent cut, persistent-thread culling, a software rasteriser for tiny triangles, and page streaming) is now reproducible from open code. meshoptimizer v1.0 ships `clusterlod.h`, and Bevy's virtual geometry is a full open implementation. For quat-3d it is a **long-term** item that depends on Q1's GPU culling and visibility buffer, plus 64-bit image atomics. Apple support for those atomics is uncertain. NVIDIA RTX Mega Geometry (VK_NV_cluster_acceleration_structure) extends the same clusters to ray tracing but is NVIDIA-only.

### Cited Findings
- Karis et al., "A Deep Dive into Nanite Virtualized Geometry" (SIGGRAPH 2021 Advances). Meshes are split into 128-triangle clusters. Clusters are grouped and simplified with group boundaries locked, then re-clustered, forming a DAG. A view-dependent LOD cut is chosen by monotonic error metrics, so parent and child decisions are independent, which permits a parallel per-cluster test. GPU persistent-thread BVH traversal and culling with two-pass HZB occlusion follow. A compute **software rasteriser** handles small triangles (claimed to beat hardware raster for pixel-sized triangles), with hardware raster for large ones. The output is a 64-bit atomic visibility buffer, and fixed-size pages are streamed. [foundational] — [Karis 2021 slides](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)
- **meshoptimizer `clusterlod.h`** is a single-header, MIT-licensed "library/example built on top of meshoptimizer to generate cluster LOD hierarchies". It was introduced with meshoptimizer **v1.0** as a "Nanite-like Continuous Level of Detail (CLOD) solution". It is kept outside the core library because such high-level algorithms are "opinionated". [verified 2026-11, from repo file and release-note mirror] — [clusterlod.h](https://raw.githubusercontent.com/zeux/meshoptimizer/master/demo/clusterlod.h); [meshoptimizer releases](https://github.com/zeux/meshoptimizer/releases); [v1.0 summary (secondary)](https://www.x-cmd.com/blog/251217)
- Kapoulkine's blog post "Billions of triangles in minutes" (2025-09-30) covers fast cluster-LOD hierarchy building. The page could not be fetched, so its throughput figures are unverified. [verified exists, contents not read] — [zeux.io](https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/)
- Community re-implementations of Nanite LOD generation exist, for example jglrxavpok (2024) and "Nanite-Style Hierarchical Meshlet LODs 101". [verified 2026-11, search] — [jglrxavpok](https://jglrxavpok.github.io/2024/01/19/recreating-nanite-lod-generation.html); [guilblack](https://guilblack.github.io/)
- **Bevy virtual geometry** (by JMS55) shipped across Bevy 0.14–0.17. Details:
  - 0.15 added a software rasteriser for small clusters, chosen by a screen-space size threshold, and the author expected nearly all clusters to end up software-rasterised. [search snippet] — [Bevy 0.15 post](https://jms55.github.io/posts/2024-11-14-virtual-geometry-bevy-0-15)
  - It needs 64-bit integers and atomics, so WebGPU was unsupported. Native Vulkan, D3D12 and Metal backends worked. [secondary, HN] — [HN thread](https://hn.nuxt.dev/item/42974461)
  - 0.16 replaced meshoptimizer's default meshlet builder because virtual geometry wants meshlets that share as few vertices as possible. BVH culling was unfinished at that point. [search snippet] — [Bevy 0.16 post](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16)
  - 0.17.3 source shows a **BVH8** used for culling and LOD selection. [verified, source listing] — [docs.rs bevy_pbr 0.17.3 meshlet/asset.rs](https://docs.rs/bevy_pbr/0.17.3/src/bevy_pbr/meshlet/asset.rs.html)
- **VK_NV_cluster_acceleration_structure** adds a BLAS type built from pre-generated clusters of primitives to cut build times. It is at revision 4 (July 2025) with a matching SPIR-V extension. [verified 2026-11] — [Vulkan docs proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_cluster_acceleration_structure.html); [SPV_NV_cluster_acceleration_structure](https://github.khronos.org/SPIRV-Registry/extensions/NV/SPV_NV_cluster_acceleration_structure.html)
- **RTX Mega Geometry Vulkan samples** (nvpro-samples):
  - `vk_animated_clusters` covers animated cluster BLAS.
  - `vk_tessellated_clusters` covers adaptive tessellation and displacement.
  - `vk_lod_clusters` covers cluster LOD and streaming.
  - `vk_partitioned_tlas` uses VK_NV_partitioned_acceleration_structure for large dynamic TLAS updates.
  - Clusters are rasterised via VK_EXT_mesh_shader.
  - The original RTXMG sample is DX12/NvAPI, labelled beta 0.9.
  [verified 2026-11] — [NVIDIA blog: RTX Mega Geometry now available with new Vulkan samples](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples); [Khronos news](https://www.khronos.org/news/permalink/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples)
- An independent developer uses cluster acceleration structures for interactive rendering of Disney's Moana Island scene (2025). [verified exists] — [mavenkim2 blog](https://mavenkim2.github.io/2025/11/09/interactive-moana-part2.html)
- Nanite shipped in UE5 titles. Fortnite (UE5.1, 2022) is the best-known case. [foundational] — [Epic Nanite docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)

### Inferences
- **Recommended path for quat-3d:**
  1. Q1 GPU culling + meshlets.
  2. Offline cluster-DAG build in the asset cooker with meshoptimizer `clusterlod.h`, which fits the existing glTF→cooked pipeline and is MIT-licensed.
  3. Runtime LOD-cut selection + two-pass occlusion in compute.
  4. Visibility buffer + material binning.
  5. Compute software rasteriser (64-bit image atomics, `VK_EXT_shader_image_atomic_int64`) for small triangles.
  6. Page streaming last.

  Each step is independently shippable. **Long-term.**
- `vk_lod_clusters` is the closest Vulkan reference for **cluster LOD + streaming** in a mesh-shader engine and complements Bevy for the visibility-buffer / software-raster side.
- Ray tracing over virtual geometry: Nanite's fallback-mesh approach is portable. RTX Mega Geometry gives exact cluster BLAS but is NV-only and unratified, so treat it as an optional NVIDIA path once quat-3d has RT at all.
- **Apple risk.** Software rasterisation needs 64-bit atomic min/max on images or buffers. Bevy reports Metal works, which suggests a Metal path exists, but MoltenVK/KosmicKrisp exposure of `shaderImageInt64Atomics` was not verified. The native Metal backend may be the realistic Apple route.

### Gaps
- Did not verify "Traverse Research" virtual geometry work. No source found, so it is not cited.
- clusterlod.h build-throughput numbers and Bevy streaming status in 0.17/0.18 are unverified because the hosts were blocked.
- No ratification of a KHR/EXT cluster-acceleration-structure extension was found. It is vendor-only (NV) as far as the search shows.

---

## Q3. Upscaling and AA: FSR 2/3/4, DLSS (Streamline), XeSS, TAAU/TSR, frame generation, integration requirements, licensing

### Takeaway
All major temporal upscalers consume the same inputs quat-3d's TAA already almost has: jittered projection, per-pixel motion vectors, depth, exposure, and optional reactive and transparency masks. Converting TAA into a single internal **upscaler interface** therefore unlocks FSR, DLSS and XeSS at once. FSR 2/3.1 is MIT-licensed source with Vulkan support and runs on every vendor including MoltenVK, so it is the **quick win**. As of the October 2026 FSR SDK 2.4, the ML FSR 4.x also has Vulkan support. DLSS SR runs on Vulkan through the NVIDIA SDK/Streamline. XeSS-SR has had Vulkan since 2.0.1. Frame generation is a separate, later decision because it adds latency and requires a latency-reduction integration.

### Cited Findings
**AMD FSR**
- FSR 2 is open source under MIT, with DX12 and Vulkan backends. It is a temporal upscaler taking colour, depth, motion vectors and exposure, with optional reactive and transparency/composition masks. FSR 3/3.1 adds frame interpolation and sits in the open-source FidelityFX SDK. [foundational] — [FidelityFX-FSR2 GitHub](https://github.com/GPUOpen-Effects/FidelityFX-FSR2); [FidelityFX SDK GitHub](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- **FSR 4** is ML-based and reached developers with **FidelityFX SDK 2.0 (August 2025)**. At that point it could not be integrated into Vulkan games. [verified 2026-11] — [GamingOnLinux Aug 2025](https://www.gamingonlinux.com/2025/08/amd-fidelityfx-super-resolution-4-fsr-4-now-available-for-all-developers-with-fidelityfx-sdk-2-0); [AMD GPUOpen SDK 2.0](https://gpuopen.com/learn/amd-fidelityfx-sdk-2-0/)
- **FSR SDK 2.4 (October 2026)**:
  - Adds **Vulkan support at feature parity with DX12** for upscaling and frame generation.
  - Ships FSR Upscaling **4.1.2**, including a full INT8 ML model for RDNA 2 (RX 6000), so RX 6000/7000/9000 and selected Ryzen APUs are supported.
  - Delivery is through the SDK's DLLs, which enables driver-side upgrades later.
  - FSR Ray Regeneration and FSR Radiance Caching remain **DX12-only**.
  - Some outlets call this the "FSR Redstone SDK 2.4"; AMD uses "FSR SDK 2.4".
  [verified 2026-11] — [GamingOnLinux Oct 2026](https://www.gamingonlinux.com/2026/10/amd-fsr-sdk-2-4-is-out-with-fsr-upscaling-4-1-2-for-older-hardware-and-vulkan-support/); [Khronos news](https://www.khronos.org/news/permalink/amd-fsr-sdk-2.4-brings-fsr-4-upscaling-and-frame-generation-to-vulkan); [Wccftech](https://wccftech.com/amd-releases-fsr-sdk-2-4-with-fsr-4-1-2-support-for-radeon-rx-6000-gpus-ryzen-apus-vulkan-support-and-improved-frame-pacing)
- Before official support, VKD3D-Proton 3.0 ran FSR 4 on Vulkan through **cooperative-matrix and float8 shader extensions**, which shows the Vulkan feature dependency. [verified 2026-11, secondary] — [Guru3D](https://www.guru3d.com/story/vkd3dproton-30-adds-amd-fsr4-antilag-supports-older-rdna-gpus/)

**NVIDIA DLSS / Streamline**
- DLSS 4 introduced **Multi Frame Generation**: up to three generated frames per rendered frame, RTX 50 only. The DLSS 4.5 SDK adds Dynamic MFG and a 6× MFG mode. Integration is through the **Streamline** SDK. [verified 2026-11] — [NVIDIA: How to integrate DLSS 4 with Streamline](https://developer.nvidia.com/blog/how-to-integrate-nvidia-dlss-4-into-your-game-with-nvidia-streamline); [NVIDIA DLSS 4.5 blog](https://developer.nvidia.com/blog/nvidia-dlss-4-5-delivers-super-resolution-upgrades-and-new-dynamic-multi-frame-generation)
- An older Streamline release (2.4.10) lists Vulkan bug fixes but states DX12 is required for DLSS Frame Generation. Current Vulkan FG/MFG status was **not confirmed** this session. [verified 2026-11, partial] — [Streamline get-started](https://developer.nvidia.com/rtx/streamline/get-started)
- The DLSS Super Resolution SDK supports Vulkan and is distributed with prebuilt binaries under an NVIDIA licence (not open source). Streamline is open source (MIT), but the DLSS feature plugins it loads are binary. [foundational] — [NVIDIA/DLSS GitHub](https://github.com/NVIDIA/DLSS); [NVIDIAGameWorks/Streamline GitHub](https://github.com/NVIDIAGameWorks/Streamline)

**Intel XeSS**
- XeSS-SR runs on any GPU with SM 6.4 / DP4a. The developer guide lists **Vulkan 1.1** with a `xess_vk.h` header. Vulkan support arrived in **v2.0.1**, and 2.1.0 improved Vulkan for XeSS-SR. XeSS 2.1 extends upscaling, FG and low-latency to NVIDIA/AMD GPUs. The SDK is closed-source binaries. The **XeSS 3 SDK is Windows-only binaries**. [verified 2026-11] — [Intel XeSS-SR Developer Guide 2.0](https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html); [github.com/intel/xess](https://github.com/intel/xess); [XeSS v2.0.1 release notes](https://newreleases.io/project/github/intel/xess/release/v2.0.1); [Tom's Hardware](https://tomshardware.com/pc-components/gpus/intel-finally-releases-the-xess-2-0-sdk-for-developers-technology-still-gated-by-closed-source-barriers); [Wccftech XeSS 3](https://wccftech.com/intel-publishes-xess-3-sdk-on-github-but-it-remains-closed-source/amp/)

**TAAU / TSR**
- UE5 Temporal Super Resolution (TSR) is Epic's engine-native, vendor-neutral temporal upscaler and the default AA/upscaler in UE5. It runs on all platforms, consoles included. [foundational] — [Epic TSR docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine)
- The classic TAA foundation is Karis, "High-Quality Temporal Supersampling" (SIGGRAPH 2014 Advances). It covers neighbourhood clamping, YCoCg and jitter. TAAU generalises it by resolving to a higher output resolution. [foundational] — [Advances 2014](https://advances.realtimerendering.com/s2014/)

### Inferences
- **Integration contract for quat-3d.** These are the inputs every upscaler needs, and the TAA/render graph should provide them. **Quick win** groundwork:
  - Sub-pixel jitter (Halton 2,3) applied in the projection matrix.
  - **Velocity buffer** including skinned and animated objects. GPU skinning output needs previous-frame positions, so keep the previous skinned buffer, which the double-buffer note already allows.
  - Render-resolution depth.
  - Exposure value or texture.
  - A reactive mask for particles and alpha-blended passes.
  - Negative mip LOD bias = log2(render/display).
  - Running post-process after the upscale.
  - An "upscaler" render-graph node whose implementation is swappable: native TAA(U), FSR 2/3.1 (source, all vendors, Linux/macOS), DLSS (NV), XeSS (Vulkan 1.1+), FSR 4 (AMD, SDK 2.4+).
- **Order:** FSR 2/3.1 upscale first (MIT, builds from source, works on MoltenVK, no binary blobs), then DLSS SR, then XeSS and FSR 4. Frame generation should wait until quat-3d has a latency-reduction integration (Reflex / Anti-Lag) and frame pacing. Generated frames increase input latency relative to rendered framerate and need UI/HUD separation.
- Licensing: FSR 2/3 source is MIT and fine to vendor. DLSS, XeSS and FSR 4 ship as binary DLLs. That suits optional runtime-loaded plugins, but it conflicts with "build everything from source" CI. Load them dynamically and keep them optional.

### Gaps
- Exact current status of DLSS Frame Generation / MFG on **Vulkan** in Streamline (late 2026) is not confirmed; check the Streamline GitHub `docs/ProgrammingGuideDLSS_G.md`.
- FSR 4 / SDK 2.4 Vulkan device-extension requirements (e.g. cooperative matrix, float8) were not read from AMD's docs because gpuopen.com was blocked.
- No measured latency figures for FG were collected in this session.

---

## Q4. Neural and ML real-time: NTC, neural materials, cooperative vectors, neural radiance caching, ML denoising/upscaling — cross-vendor Vulkan readiness

### Takeaway
Neural shading techniques (NTC, neural materials, NRC) are technically mature. They are published at SIGGRAPH and NTC has an open SDK. The **API layer is still fragmenting**:
- Vulkan has only **VK_NV_cooperative_vector** (NVIDIA, not ratified) plus cross-vendor **VK_KHR_cooperative_matrix**.
- DirectX deprecated its Cooperative Vector in favour of a unified **DirectX Linear Algebra** (SM 6.10, previewed April 2026).

For a cross-vendor Vulkan + Metal engine in late 2026, neural shading is **long-term / experimental**: keep an NV-only experiment behind a feature flag, and track Khronos for a KHR/EXT cooperative-vector equivalent.

### Cited Findings
- **VK_NV_cooperative_vector** adds SPIR-V cooperative vector types to accelerate *small neural networks*, where each invocation evaluates its own network while the hardware cooperates on the matrix-vector multiply. It is registered extension 492, revision 4 (2024-05-23), **not ratified**, with layouts for inference- and training-optimal weights. [verified 2026-11] — [Vulkan docs](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_cooperative_vector.html); [proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_cooperative_vector.html)
- A Khronos ML talk places the cooperative-vector building block on a path toward "cross-IHV ratification". [verified 2026-11] — [Khronos "Machine Learning, Neural Rendering and the Future of Open Graphics" slides](https://www.khronos.org/developers/linkto/machine-learning-neural-rendering-and-the-future-of-open-graphics-slides)
- Vulkan ML extension activity through 2026, with no KHR cooperative vector found:
  - VK_KHR_cooperative_matrix is ratified and cross-vendor.
  - Vulkan 1.4.352 (May 2026) added VK_NV_cooperative_matrix_decode_vector.
  - Vulkan 1.4.359 (Aug 2026) added VK_EXT_cooperative_matrix_maintenance1.
  - Qualcomm proposed VK_QCOM_cooperative_matrix_conversion (Vulkanised 2026).
  [verified 2026-11] — [VK_KHR_cooperative_matrix](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_cooperative_matrix.html); [Phoronix 1.4.352](https://phoronix.com/news/vulkan-1.4.352-released); [Phoronix 1.4.359](https://phoronix.com/news/Vulkan-1.4.359); [Qualcomm Vulkanised 2026](https://vulkan.org/user/pages/09.events/vulkanised-2026/1000-Amir-Momeni-Qualcomm.pptx.pdf)
- **DirectX:**
  - Cooperative Vector was previewed for SM 6.9 (June 2025).
  - In September 2025 Microsoft said the unified design would not ship in SM 6.9 and that the original Cooperative Vector spec would be deprecated.
  - SM 6.9 shipped in Feb 2026 with Long Vector but no Cooperative Vector.
  - **DirectX Linear Algebra** (matrix-matrix + vector-matrix), announced at GDC 2026, has commitments from AMD, Intel, NVIDIA and Qualcomm and entered public preview in April 2026, targeting SM 6.10.
  [verified 2026-11] — [DirectX blog: SM 6.9 and the future of Cooperative Vector](https://devblogs.microsoft.com/directx/shader-model-6-9-and-the-future-of-cooperative-vector/); [SM 6.9 retail](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/); [GDC 2026: Evolving DirectX for the ML era](https://developer.microsoft.com/en-us/games/articles/2026/03/gdc-2026-evolving-directx-for-ml-era-on-windows/)
- NVIDIA says cooperative vector APIs are being introduced in OptiX, DirectX, NVAPI, **Slang** and Vulkan, and that they use RTX Tensor Cores. [verified 2026-11] — [NVIDIA blog: Neural rendering in OptiX using cooperative vectors](https://developer.nvidia.com/blog/neural-rendering-in-nvidia-optix-using-cooperative-vectors)
- **Neural Texture Compression (NTC):**
  - Research basis: Vaidyanathan et al., "Random-Access Neural Compression of Material Textures" (SIGGRAPH 2023). [foundational] — [NVIDIA research page](https://research.nvidia.com/labs/rtr/neural_texture_compression/)
  - The **RTXNTC SDK** (open on GitHub) supports Vulkan 1.3. Its Vulkan path uses VK_NV_cooperative_vector, and D3D12 uses Microsoft LinAlg (SM 6.10).
  - It has a DP4a/integer fallback (`--no-coopVec`) described as significantly slower and meant for validation.
  - Acceleration targets Ada and Blackwell.
  - Modes on Vulkan are *Inference on Load* (decompress to BCn at load) and *Inference on Sample*. *Inference on Feedback* is unsupported on Vulkan.
  - Tom's Hardware reports VRAM reductions of over 80%.
  [verified 2026-11] — [github.com/NVIDIA-RTX/RTXNTC](https://github.com/NVIDIA-RTX/RTXNTC); [Phoronix RTXNTC beta](https://www.phoronix.com/news/NVIDIA-RTXNTC-0.5-Beta/); [Tom's Hardware benchmark](https://www.tomshardware.com/pc-components/gpus/benchmarking-nvidias-rtx-neural-texture-compression-tech-that-can-reduce-vram-usage-by-over-80-percent)
- **Neural materials:** Zeltner et al., "Real-Time Neural Appearance Models" (ACM TOG, SIGGRAPH 2024; arXiv 2305.02678). Layered material graphs are baked into learned hierarchical latent textures plus small MLP decoders with graphics priors (learned shading frames, microfacet sampling distribution for importance sampling). The decoders run inside a real-time path tracer through tensor ops exposed to RT shaders. NVIDIA claims they are **over an order of magnitude faster** than non-neural layered materials. [verified 2026-11] — [NVIDIA research page](https://research.nvidia.com/publication/_real-time-neural-appearance-models); [arXiv 2305.02678](https://arxiv.org/pdf/2305.02678)
- **Neural Radiance Caching:** Müller et al., "Real-time Neural Radiance Caching for Path Tracing" (SIGGRAPH 2021). A small MLP trained online caches radiance for path termination. Evolve Entertainment describes adopting neural materials (2021) and then NRC, using cooperative vectors to handle divergent per-pixel networks. [foundational: paper; verified: Evolve] — [NVIDIA research](https://research.nvidia.com/publication/2021-06_real-time-neural-radiance-caching-path-tracing); [GameDev.net cooperative vectors intro](https://gamedev.net/news/3317-cooperative-vectors-introduction/)
- AMD's FSR "Redstone" ML features (Ray Regeneration = ML denoiser, Radiance Caching) remain DX12-only as of SDK 2.4. [verified 2026-11] — [GamingOnLinux Oct 2026](https://www.gamingonlinux.com/2026/10/amd-fsr-sdk-2-4-is-out-with-fsr-upscaling-4-1-2-for-older-hardware-and-vulkan-support/)

### Inferences
- **Readiness for quat-3d (cross-vendor Vulkan + Metal):**
  - *Not ready* for default-on neural shading. The only Vulkan inline-MLP path is NV-only and unratified. AMD's ML shading is DX12-first. Apple is a separate stack.
  - *Ready* for "ML via vendor SDK" features: DLSS, FSR 4 and XeSS through the upscaler interface (Q3).
  - *Experimental:* NTC "Inference on Load" is the lowest-risk entry point. It produces ordinary BCn at load time, so runtime rendering is unchanged, and it gives disk/download savings on any GPU that can run the decoder, including the slow DP4a fallback.
- Slang already supports cooperative-vector intrinsics (per NVIDIA), which matches quat-3d's shader language. Writing a tiny MLP decoder in Slang with a scalar fallback path is a feasible research spike.
- Watch item: a KHR/EXT cooperative-vector or "linear algebra" Vulkan extension mirroring DirectX Linear Algebra. Re-evaluate neural materials and NRC when one ships.

### Gaps
- No confirmed KHR/EXT cooperative-vector Vulkan extension as of Nov 2026; searches found none.
- No shipped game confirmed using NTC Inference-on-Sample or neural materials in production.
- ML denoisers (DLSS Ray Reconstruction, FSR Ray Regeneration) were not researched in depth. quat-3d has no ray tracing yet, so they are low priority.

---

## Q5. Texture and mesh pipeline: BC7/ASTC, KTX2 + Basis Universal, meshoptimizer compression, virtual texturing and sparse residency

### Takeaway
The modern, portable texture pipeline is **KTX2 containers** with either **Basis Universal supercompression** (UASTC or ETC1S, transcoded at load to BC7/BC5/BC1 on desktop or ASTC on Apple) or offline-encoded BC7/ASTC. glTF supports this through `KHR_texture_basisu`. Meshes should use meshoptimizer's vertex/index codecs (`EXT_meshopt_compression`). Both are **quick-to-medium wins** that fit the existing cooker. Virtual texturing is long-term. Hardware sparse residency is the weakest cross-platform option: KosmicKrisp lists sparse memory as future work.

### Cited Findings
- KTX 2.0 is the Khronos GPU-texture container with Basis Universal supercompression (ETC1S and UASTC), plus Zstd and mip streaming. It is the format behind glTF `KHR_texture_basisu`. [foundational] — [KhronosGroup/KTX-Software](https://github.com/KhronosGroup/KTX-Software); [glTF KHR_texture_basisu](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_basisu)
- Basis Universal (Binomial, Apache-2.0) transcodes supercompressed textures to BC1–BC7, ASTC, ETC and others at runtime, so one asset serves desktop BC and Apple ASTC. [foundational] — [BinomialLLC/basis_universal](https://github.com/BinomialLLC/basis_universal)
- meshoptimizer provides vertex/index buffer codecs (`meshopt_encodeVertexBuffer` / `meshopt_encodeIndexBuffer`). They decode at GB/s on the CPU, are designed to follow general-purpose compression (zstd), and are standardised in glTF as `EXT_meshopt_compression` (via gltfpack). [foundational] — [meshoptimizer README](https://github.com/zeux/meshoptimizer); [EXT_meshopt_compression](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Vendor/EXT_meshopt_compression)
- Virtual texturing at scale shipped in id Tech 5 (*Rage*) and Ubisoft's GPU-driven pipeline (Haar & Aaltonen 2015). Both used software page tables plus a feedback pass, not hardware sparse textures. [foundational] — [Haar & Aaltonen 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)
- KosmicKrisp lists **sparse memory** as future work, so sparse residency (`sparseResidencyImage2D`) cannot be assumed on Apple Vulkan drivers. [verified 2026-11] — [LunarG XDC 2025](https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf)
- NTC is positioned as a successor to BCn for material texture sets. It requires an NN-capable decoder path (see Q4). [verified 2026-11] — [RTXNTC](https://github.com/NVIDIA-RTX/RTXNTC)

### Inferences
- **Quick win:** make the cooker emit KTX2 (UASTC + Zstd for normals and high-quality maps, ETC1S for low-priority maps, or direct BC7/BC5). Transcode to BC on desktop and ASTC on Apple at load. This works with fastgltf's `KHR_texture_basisu` support and with the asset manager's async loading.
- **Quick win:** apply meshoptimizer's vertex cache/fetch optimisation and quantisation at cook time, and its vertex/index codecs for on-disk compression. It is the same library needed for meshlets (Q1) and cluster LOD (Q2), so there is one dependency for the whole geometry track.
- **Long-term:** software virtual texturing (feedback buffer → page requests → physical cache atlas + indirection texture) is portable to Metal. Prefer it over hardware sparse residency given the MoltenVK/KosmicKrisp gaps.

### Gaps
- Did not verify the current MoltenVK sparse-binding/residency status.
- No 2024–2026 benchmark comparing Basis UASTC vs offline BC7 quality and size was gathered this session.

---

### Summary priority table (synthesis of the above; inferences, not sourced claims)

| Item | Requirements | MoltenVK/Apple | Priority |
|---|---|---|---|
| GPU instance frustum + two-pass Hi-Z occlusion, `DrawIndexedIndirectCount` | Vulkan 1.2 core, BDA | Yes | **Medium, next** (planned GPU culling) |
| Meshlets + compute cluster culling (meshoptimizer) | Core compute | Yes | Medium |
| VK_EXT_mesh_shader fast path | EXT mesh shader | Not confirmed on MoltenVK; KosmicKrisp "future" | Medium-later, feature-guarded |
| Upscaler interface + FSR 2/3.1 (MIT) | Jitter, velocity, reactive mask | Yes | **Quick win** |
| DLSS SR / XeSS-SR / FSR 4.1 plugins | Vendor binaries; FSR 4 Vulkan needs SDK 2.4 | No | Medium |
| Frame generation | Latency reduction, pacing, UI split | No | Long-term / optional |
| Visibility buffer + material binning | 64-bit atomics (for SW raster) | Uncertain | Medium-long |
| Cluster DAG LOD (clusterlod.h) + SW raster + streaming | Above + image int64 atomics | Uncertain | Long-term |
| RTX Mega Geometry cluster BLAS | VK_NV_cluster_acceleration_structure | No | Long-term, NV-only |
| KTX2/Basis + meshopt codecs | None special | Yes | **Quick win** |
| NTC (Inference on Load) | DP4a fallback / NV coop-vector | Fallback only | Experimental |
| Neural materials / NRC | NV coop-vector (unratified) | No | Watch list |
