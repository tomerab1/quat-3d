# Lay shared foundations before frontier rendering features

quat-3d gets the most from a small set of shared GPU building blocks and well-proven 2014–2020 techniques, not from frontier features such as Nanite-style virtual geometry, path tracing or neural shading. The building blocks are:

- a Hi-Z depth pyramid;
- a complete per-pixel velocity buffer behind one upscaler interface;
- one global wind and `WeatherState` field;
- 4-cascade shadow maps;
- GPU instance culling with indirect draws;
- a GBuffer shading-model ID.

On top of those sit cheap, proven techniques: GTAO and its visibility-bitmask successor, contact shadows, FSR 2/3.1, froxel fog, 2.5D Nubis-style clouds, GPU particles with weighted-blended OIT, inertialization, foot IK and ACL animation compression.

The macOS target sets the hard limit on what can be on by default. Neither MoltenVK (v1.4.2, July 2026) nor KosmicKrisp (Vulkan 1.4 conformant, October 2026) exposes ray queries, and neither confirms mesh shaders or sparse residency. Every default-on technique therefore needs a compute-only path. Hardware ray tracing, mesh shaders, NVIDIA cluster acceleration structures and cooperative-vector neural shading stay optional tiers for Windows and Linux.

For global illumination (GI), the right design is the one Lumen and AMD both converged on: one world-space radiance cache, fed by two interchangeable tracers. One tracer is compute-only, using a signed distance field (SDF) of the scene in the style of AMD's MIT-licensed Brixelizer GI. The other uses `VK_KHR_ray_query`.

The engine's AI-driven world building also shapes the design. Each feature should be exposed as declarative, validated data that MCP tools can author and that screenshots can verify: weather presets, biome recipes, wind emitters, fog volumes, destruction fields and emitter descriptors. Raw shader knobs are not enough.

This report has one significant caveat. Many primary hosts (GDC Vault, the Advances course site, JCGT, Epic docs, GPUOpen, Guerrilla, EA) were blocked during research. Some details therefore rest on search-result extracts or background knowledge. Those claims are flagged in the text and listed in the final section.

## macOS has no ray query, so every default feature needs a compute path

Platform availability decides more of this roadmap than any single algorithm. MoltenVK's changelog, from v1.2.8 through v1.4.2 (released 2026-07-20) and the in-progress v1.4.3, contains no ray-tracing or acceleration-structure entries ([MoltenVK Whats_New.md](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md)). Its maintainer listed "initial ray tracing" only as an unfunded work item at Vulkanised 2024 ([Vulkanised 2024 slides](https://dev.vulkan.org/user/pages/09.events/vulkanised-2024/vulkansied-2024-bill-hollings-brenwill.pdf)). The translation layer is not what blocks it: SPIRV-Cross can already lower `SPV_KHR_ray_query` to Metal intersection queries ([SPIRV-Cross commit](https://fuchsia.googlesource.com/third_party/spirv-cross/+/597f29d09d3d49fca9f3654c43210ca0cb3b5371%5E%21/)). MoltenVK simply does not expose the extension.

LunarG's Mesa-based KosmicKrisp passed **Vulkan 1.4 conformance in October 2026** on Apple Silicon running macOS 26 ([Khronos](https://www.khronos.org/news/permalink/kosmickrisp-achieves-vulkan-1.4-conformance-on-apple-silicon); [Phoronix](https://www.phoronix.com/news/KosmicKrisp-Vulkan-1.4-2026)). Its own roadmap still lists sparse memory, tessellation, **mesh shaders and "potentially ray tracing"** as future goals ([LunarG XDC 2025](https://www.lunarg.com/lunarg-at-xdc-2025-kosmickrisp-overview/)). Wikipedia gives a different MoltenVK 1.4.2 release date (November 2025). The repository changelog is authoritative.

The same split applies to the most advanced geometry and ML features. RTX Mega Geometry's cluster and partitioned acceleration structures are `VK_NV_*` extensions, not ratified across vendors ([NVIDIA blog](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples)). Microsoft only previewed the DXR equivalents (CLAS/PTLAS) on 28 October 2026 ([DirectX blog](https://devblogs.microsoft.com/directx/clas-ptlas-preview/)). The only Vulkan path for running small MLPs inside shaders is the unratified `VK_NV_cooperative_vector` ([Vulkan docs](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_cooperative_vector.html)).

The practical rule follows directly. Default-on features must use only core Vulkan 1.3 compute, indirect draws, 3D textures and 32-bit atomics. Everything else is a feature-checked tier with a fallback. The native Metal backend (ROADMAP P2/P5) is the realistic way for Apple to get ray tracing and mesh shaders later.

| Capability | Vulkan feature | Windows/Linux | macOS today | Techniques that depend on it |
|---|---|---|---|---|
| Compute, indirect count draws, BDA, 3D textures | Core 1.2/1.3 | Yes | Yes | GTAO, SSR, CSM/SDSM, froxel fog, clouds, FFT ocean, particles, GPU culling, SDF GI, grass, placement |
| Hardware ray tracing | `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` | RT-capable GPUs | **No** (MoltenVK, KosmicKrisp) | RT shadows/AO, HW-traced DDGI, ReSTIR, path tracing |
| Mesh/task shaders | `VK_EXT_mesh_shader` | Most desktop GPUs | Not confirmed; KosmicKrisp "future" | Meshlet fast path, mesh-shader fur shells, cluster foliage |
| 64-bit image/buffer atomics | `VK_EXT_shader_image_atomic_int64`, `shaderBufferInt64Atomics` | Yes | Unverified | Software rasterisation (virtual geometry, strand hair), R64 visibility buffer |
| Sparse residency | `sparseResidencyImage2D` | Yes | **No** on KosmicKrisp | Hardware virtual texturing, sparse shadow pages |
| Cluster AS / partitioned TLAS | `VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure` | NVIDIA only | No | RT over virtual geometry and foliage |
| Inline MLPs | `VK_NV_cooperative_vector` (unratified); `VK_KHR_cooperative_matrix` (ratified) | NVIDIA (vector); cross-vendor (matrix) | No | Neural textures on sample, neural materials, NRC |

The table also determines what can be tested. Headless continuous integration (CI) on lavapipe has no ray tracing, so the compute tiers are the only ones regression tests can exercise. Building them first also gives the engine its test coverage first.

## Lighting: fix shadows and AO first, then one GI cache with two tracers

quat-3d's lighting already has deferred shading, GPU-clustered lights, TAA, IBL and Hillaire-style atmosphere LUTs. It has three gaps. It renders a single directional shadow map (`src/renderer/shadow_pass.cpp`), although CLAUDE.md specifies four cascades. No AO, SSR or GI shader exists in `assets/shaders/`. And TAA reprojects only from depth and camera matrices (`assets/shaders/taa.slang`), with no per-object velocity, so anything moving, skinned or wind-animated will smear. These gaps come first, because GI multiplies whatever errors exist in direct lighting and occlusion.

### Screen-space occlusion and better shadows are the cheapest quality jump

GTAO (Jimenez et al., SIGGRAPH 2016) is the reference real-time AO. Intel's MIT-licensed XeGTAO implements it in three compute passes: depth prefilter, main pass and a 5×5 denoise, leaving temporal accumulation to TAA. Its High preset costs **about 0.56 ms at 1080p on an RTX 2060** and about 1.4 ms at 4K on an RTX 3070. Optional bent normals add about 25%. Note that **Intel archived the repository in April 2024**, so a Slang port means owning the code ([XeGTAO](https://github.com/GameTechDev/XeGTAO)).

The 2023 visibility-bitmask variant (SSILVB) goes further. It replaces GTAO's two horizon angles with a sector bitmask plus a constant thickness, so light can pass behind thin surfaces. The same horizon walk then yields AO plus short-range indirect diffuse at low overhead ([arXiv 2301.11376](https://arxiv.org/abs/2301.11376)). Stochastic SSR (Stachowiak, SIGGRAPH 2015 Advances; productised as AMD FidelityFX SSSR) and short screen-space contact-shadow marches complete the screen-space layer. Both are flagged unverified, because their primary decks were not fetched.

Every screen-space technique here needs a Hi-Z depth pyramid. So does R6 occlusion culling, which makes Hi-Z the first item to build.

For shadows, the industry baseline is four cascades, either stable (texel-snapped) or depth-bounds-driven. Sample Distribution Shadow Maps (SDSM) fit the partitions to the depth samples the current frame actually needs ([Intel SDSM](https://www.intel.com/content/www/us/en/developer/articles/technical/sample-distribution-shadow-maps.html)). Filtering is PCF/PCSS, or a moment-based method: Moment Shadow Mapping stores four moments in 64 bits per texel and needs only one filtered sample ([Peters & Klein, I3D 2015](https://momentsingraphics.de/I3D2015.html)). MJP's survey benchmarks the options ([A Sampling of Shadow Techniques](https://therealmjp.github.io/posts/shadow-maps)).

UE5's virtual shadow maps are the high end: 16k² clipmaps paged in 128² tiles and cached across frames. They depend on GPU-driven geometry submission. Those details come from Epic docs that could not be fetched ([Epic VSM docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)).

For quat-3d, the depth-bounds reduction should run in compute and write the cascade matrices to a buffer, so there is no CPU readback.

| Technique | Requirements / macOS | Cost (estimate unless cited) | Fit in quat-3d | Priority |
|---|---|---|---|---|
| Hi-Z depth pyramid (single-pass min/max reduction) | Core compute / yes | Low | One render-graph resource shared by R6, GTAO, SSR, contact shadows, foliage cull | **Quick win** |
| GTAO (XeGTAO port) + bent normals, then SSILVB | Core / yes | ~0.5–1.5 ms ([XeGTAO](https://github.com/GameTechDev/XeGTAO)) | Compute after GBuffer; bent normals give IBL specular occlusion; TAA accumulates | **Quick win (R8)** |
| Contact shadows | Core / yes | Low | Short march in lighting pass; fixes foliage/character detail CSM misses | **Quick win (R8)** |
| 4-cascade CSM, GPU depth bounds, texel snapping, PCF/PCSS (MSM optional) | Core / yes | Medium | Replaces single map; prerequisite for all GI injection | **Quick win → medium** |
| Stochastic SSR (Hi-Z, SSSR-style) with probe/IBL fallback | Core / yes | Medium; ~1–2 ms unverified | Needs Hi-Z + TAA; falls back to dynamic IBL | **Medium (R8)** |
| Virtual shadow maps | Core + GPU-driven draws; sparse helpful / probably | High | Only after R5/R6/R9.4 | **Long-term** |

### Probe GI and SDF tracing give macOS dynamic GI without ray tracing

Dynamic Diffuse Global Illumination (DDGI) is a world-space probe grid. Each probe stores octahedral irradiance plus depth moments, and a Chebyshev visibility test suppresses leaks. A few hundred rays per probe per frame keep it current ([JCGT 2019](https://jcgt.org/published/0008/02/01/), unverified because the host was blocked). It is world-space, so it stays stable under TAA. Its update is a fixed-size compute workload. Only the probe-ray step needs ray tracing, and that step can be swapped out.

NVIDIA has since split DDGI into a separate v1.x repository. RTXGI 2.0 now centres on two path-tracing radiance caches: the Spatially Hashed Radiance Cache (SHaRC) and the Neural Radiance Cache (NRC) ([RTXGI listing](https://awesome.ecosyste.ms/projects/github.com%2FNVIDIA-RTX%2FRTXGI)). Press coverage contradicts the release notes about whether DDGI was a 2.0 headline feature ([Wccftech](https://wccftech.com/nvidia-rtxgi-2-0-available-next-frontier-ray-traced-visuals-neural-radiance-cache-spatial-hash-radiance-cache-dynamic-diffuse-global-illumination/)).

Every major software GI system since 2021 follows the same three-part recipe: a cheap scene representation, a lighting cache on top of it, and a screen-probe final gather with temporal reuse. Lumen is UE5's default GI. In Fortnite at 60 fps it had **about 4 ms for GI and reflections combined**, and needed software tracing to hit that budget ([Epic: Lumen in Fortnite](https://www.unrealengine.com/en-US/tech-blog/lumen-brings-real-time-global-illumination-to-fortnite-battle-royale-chapter-4)). AMD's GI-1.0 pairs a screen cache with a hash-based world cache and runs in **1.9–3.1 ms on an RX 6900 XT** using hardware ray tracing ([arXiv 2310.19855](https://ar5iv.labs.arxiv.org/html/2310.19855)).

The most adoptable reference is **AMD FidelityFX Brixelizer GI**. It is a simplified GI-1.0 that traces sparse distance-field bricks (64³-voxel cascades) "in compute without hardware-accelerated ray-tracing". It outputs denoised diffuse and specular, and it is **MIT-licensed for DX12 and Vulkan** ([GPUOpen Brixelizer](https://gpuopen.com/fidelityfx-brixelizer/); [Introducing Brixelizer](https://gpuopen.com/learn/introducing-amd-fidelityfx-brixelizer/)). AMD warns that the SDF's low frequency causes inaccuracies and can need a larger ray bias.

Godot ships SDFGI as a no-bake, no-RT reference ([Godot blog](https://godotengine.org/article/godot-40-gets-sdf-based-real-time-global-illumination/)). Its planned successor, HDDAGI, reportedly cut one trace pass from about 2.4 to 0.9 ms on a GTX 1650 ([GameFromScratch](https://gamefromscratch.com/godot-new-global-illumination-system-hddagi/)). Voxel cone tracing has been superseded by SDF-plus-probe caches and should be avoided.

For quat-3d, the key design decision is a **single GI interface with pluggable tracers**. Lumen (software and hardware paths) and AMD (GI-1.0 hardware tracing, Brixelizer GI compute SDF) both keep one cache and gather architecture and swap only the trace step. quat-3d should do the same, which avoids separate GI forks per platform. Doing so also keeps Vulkan types out of renderer-facing APIs, as the P2 backend seam requires. Terrain should be ray-marched analytically against the heightfield rather than voxelised into the SDF, which avoids blowing up SDF memory over large terrain.

| Tier | Contents | Platforms | Priority |
|---|---|---|---|
| 0 | CSM + GTAO/SSILVB + IBL + baked or sky-relit L1/L2 SH probe volume | All, incl. iGPU and macOS | Quick win → medium |
| 1 | DDGI-style probe volume (camera-relative cascades, relocation, classification) traced against a global sparse SDF; SSR | All desktop + macOS (64-bit atomic needs unverified) | Long-term (main dynamic GI); probe data structure is medium |
| 1+ | Screen-probe final gather + world radiance cache (GI-1.0/Lumen style) | Same | Long-term |
| 2 | Ray-query tracer behind the same cache; BLAS/TLAS build and skinned-BLAS refit after the skinning pass; RT sun shadows and AO | Windows/Linux RT GPUs | Medium-long |
| 3 | ReSTIR DI (froxel light lists as candidates), ReSTIR GI, SHaRC, path-traced showcase mode | High-end PC | Long-term |

### Hardware ray tracing should start with ray queries in compute

`VK_KHR_ray_query` traces from any shader stage without a shader binding table ([Khronos](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_ray_query.html)). That suits a compute-first render graph better than the full ray-tracing pipeline. The first uses should be RT directional shadows, RT AO and DDGI probe tracing.

These need denoising. NVIDIA's NRD lists measured costs at 1440p on an RTX 4080 ([NRD README](https://github.com/NVIDIA-RTX/NRD)):

| NRD denoiser | Cost |
|---|---|
| SIGMA (shadows) | **0.40 ms** |
| REBLUR diffuse+specular | 2.55 ms |
| RELAX diffuse+specular | 3.25 ms |

NRD supports Vulkan through SPIR-V binding offsets. Its shaders are HLSL, so the team must choose between vendoring NRD and writing an SVGF-class denoiser in Slang.

Beyond that, the high end is reservoir resampling. ReSTIR DI (SIGGRAPH 2020) is productised as RTXDI. ReSTIR GI reports **9.3×–166× lower MSE** at one path per pixel ([NVIDIA Research](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing)). Cyberpunk 2077's RT Overdrive combined ReSTIR, radiance caching and DLSS Ray Reconstruction and recommended RTX 40-series hardware ([NVIDIA blog](https://blogs.nvidia.com/blog/2023/03/22/cyberpunk-2077-path-traced-visuals-gdc/)). It also kept volumetrics and forward-rendered elements outside the path tracer ([Wccftech Q&A](https://wccftech.com/cyberpunk-2077-path-tracing-q-a-plenty-room-improve/amp/)). That is a showcase tier, not a foundation.

### Upscaling and AA: one jitter-and-velocity contract unlocks every vendor

Every modern temporal upscaler consumes the same inputs:

- jittered projection;
- per-pixel motion vectors;
- depth;
- exposure;
- optional reactive and transparency masks.

That set has been the foundation since Karis's 2014 temporal supersampling talk ([Advances 2014](https://advances.realtimerendering.com/s2014/)). quat-3d's TAA already has jitter and history. Its missing piece is **object velocity**. GPU skinning must keep the previous frame's skinned positions, and foliage wind, particles and water must output previous-frame displacement.

Once those inputs exist, one "upscaler" render-graph node can host any implementation:

- **FSR 2/3.1** is MIT-licensed source with a Vulkan backend and runs on every vendor, MoltenVK included ([FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)). That makes it the quick win.
- **FSR SDK 2.4** (October 2026) brought FSR 4's ML upscaler and frame generation to Vulkan at parity with DX12. It includes an INT8 model for RX 6000 GPUs. FSR Ray Regeneration and Radiance Caching remain DX12-only ([GamingOnLinux](https://www.gamingonlinux.com/2026/10/amd-fsr-sdk-2-4-is-out-with-fsr-upscaling-4-1-2-for-older-hardware-and-vulkan-support/); [Khronos](https://www.khronos.org/news/permalink/amd-fsr-sdk-2.4-brings-fsr-4-upscaling-and-frame-generation-to-vulkan)).
- **DLSS Super Resolution** supports Vulkan through binary SDKs behind the MIT-licensed Streamline. DLSS 4 Multi Frame Generation is RTX 50-only, and the DLSS 4.5 SDK adds a dynamic 6× mode ([NVIDIA DLSS 4.5](https://developer.nvidia.com/blog/nvidia-dlss-4-5-delivers-super-resolution-upgrades-and-new-dynamic-multi-frame-generation)). Whether frame generation currently works on Vulkan was not confirmed; an older Streamline release required DX12 for it.
- **XeSS-SR** has supported Vulkan since 2.0.1, as closed binaries ([Intel XeSS guide](https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html)). The XeSS 3 SDK is Windows-only binaries.

Binary SDKs conflict with a build-from-source CI, so they belong in runtime-loaded optional plugins. Frame generation adds latency. It should wait until quat-3d has latency reduction (Reflex/Anti-Lag), frame pacing and HUD separation.

| Technique | Requirements / macOS | Cost | Fit | Priority |
|---|---|---|---|---|
| Complete velocity buffer (skinned, instanced, wind, particles) + reactive mask + negative mip bias | Core / yes | Low–medium | Prerequisite for TAA quality, all upscalers, foliage, clouds | **Quick win** |
| Upscaler node + native TAAU + FSR 2/3.1 (MIT) | Core / yes | Medium | Post-process after upscale; swappable implementation | **Quick win** |
| DLSS SR, XeSS-SR, FSR 4.1 plugins | Vendor binaries / no | Medium | Same node, dynamically loaded | **Medium** |
| Frame generation (FSR FG, DLSS MFG) | Latency stack, UI split / no | Medium–high | After pacing work | **Long-term / optional** |

## Geometry and foliage: GPU culling first, virtual geometry last

### Compute-driven culling is the geometry step every later feature needs

The established GPU-driven pipeline works like this. Compute culls instances and 64–128-triangle clusters against the frustum and a Hi-Z pyramid, then writes compacted draw commands that `vkCmdDrawIndexedIndirectCount` consumes. The design comes from Haar and Aaltonen's Assassin's Creed Unity talk ([Advances 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)) and Wihlidal's Frostbite triangle-culling work ([GDC 2016](https://www.gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With)).

Occlusion culling should use the two-pass Hi-Z form:

1. Draw last frame's visible set.
2. Rebuild the pyramid from that depth.
3. Re-test everything else and draw the newly visible objects.

This removes reprojection popping, and Nanite uses the same scheme ([Karis 2021](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)). Arseny Kapoulkine's open-source Vulkan renderer **niagara** implements exactly this stack: meshlets, a mesh-shader path with a traditional fallback, two-phase occlusion and indirect-count draws ([niagara](https://github.com/zeux/niagara)). It is the closest code reference.

Everything in this stack needs only core Vulkan 1.2/1.3. quat-3d already reads per-instance transforms through buffer device addresses and uses descriptor buffers, so what remains is a set of compute passes. That is R6 plus R9.4, which also replaces today's per-instance CPU culling.

Meshlets built with meshoptimizer (`meshopt_buildMeshlets` plus bounds and normal cones, MIT) can be culled in compute and drawn through the same indirect path ([meshoptimizer](https://github.com/zeux/meshoptimizer)). A `VK_EXT_mesh_shader` fast path can then be added behind the feature check CLAUDE.md already requires ([Khronos proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html)).

Virtualised geometry is now reproducible from open code. meshoptimizer v1.0 ships `clusterlod.h`, a Nanite-like cluster-LOD DAG builder ([clusterlod.h](https://raw.githubusercontent.com/zeux/meshoptimizer/master/demo/clusterlod.h)). Bevy shipped a complete implementation with a software rasteriser, a BVH8 and an R64 visibility buffer using texture atomics ([Bevy 0.16](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16); [bevy_pbr source](https://docs.rs/bevy_pbr/0.17.3/src/bevy_pbr/meshlet/asset.rs.html)).

Virtual geometry also depends on a visibility buffer ([Burns & Hunt, JCGT 2013](https://jcgt.org/published/0002/02/04/)), a software rasteriser and 64-bit atomics, and Apple exposure of those atomics is unverified. That makes it a long-term item, built in independently shippable steps: GPU culling, a cooker-side cluster DAG, runtime LOD cut, visibility buffer with material binning, software raster, and streaming last.

The same meshoptimizer dependency also provides vertex and index codecs (`EXT_meshopt_compression`). With KTX2 and Basis Universal transcoding, one asset can serve BC on desktop and ASTC on Apple ([KTX-Software](https://github.com/KhronosGroup/KTX-Software); [Basis Universal](https://github.com/BinomialLLC/basis_universal)). Both are cheap additions to the cooker. Virtual texturing should be the software page-table kind, not hardware sparse residency, which KosmicKrisp lacks ([LunarG XDC 2025 slides](https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf)).

| Technique | Requirements / macOS | Cost | Fit | Priority |
|---|---|---|---|---|
| GPU instance frustum + two-pass Hi-Z cull, `DrawIndexedIndirectCount`, LOD select | Core 1.2 / yes | Medium | R6 + R9.4 + R5; reuses BDA instance buffers | **Medium, start next** |
| Meshlets + compute cluster culling (meshoptimizer) | Core / yes | Medium | One data format for compute and mesh-shader paths | **Medium** |
| `VK_EXT_mesh_shader` fast path | EXT mesh shader / unconfirmed | Medium | Feature-guarded acceleration only | **Medium-later** |
| KTX2/Basis textures + meshopt codecs and quantisation | None / yes | Low | Cooker + fastgltf `KHR_texture_basisu` | **Quick win** |
| Visibility buffer + material binning | 64-bit atomics for SW raster / uncertain | High | Slang interfaces map to material-bin dispatches | **Medium-long** |
| Cluster DAG LOD + SW raster + page streaming | int64 image atomics / uncertain | XL | After all of the above | **Long-term** |
| Software virtual texturing | Core / yes | High | Portable to Metal | **Long-term** |
| RTX Mega Geometry cluster BLAS / PTLAS | NV-only | XL | Only once RT tier exists | **Watch** |

### Foliage quality comes from four shared GPU systems, not from a single technique

Shipped vegetation shows three tiers of wind. The first is cheap per-vertex bending: Crysis's main plus detail bending, with smoothed triangle waves standing in for sines ([GPU Gems 3 ch.16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)), and SpeedTree's ripple and branch levels with a per-instance "independence" de-sync ([SpeedTree wind config](https://docs.unity3d.com/speedtree-runtime-sdk/manual/wind-shader-configuration.html)). The second is hierarchical rigid-pivot bending from baked per-element pivot and parent data, as in Epic's Pivot Painter 2 ([Epic docs](https://dev.epicgames.com/documentation/unreal-engine/pivot-painter-tool-2.0-in-unreal-engine)). The third is joint-driven plants sampling a simulated field. Ghost of Tsushima rigged plants with joints that respond to local wind. One gust noise drove trees, grass, rope and cloth together, and up to **60,000 leaf particles** blew at once ([PlayStation Blog](https://blog.playstation.com/?p=345372)).

The simulated fields themselves vary. Tsushima used vorticles, favouring "volume over accuracy" ([Game Developer](https://gamedeveloper.com/design/using-vorticles-to-simulate-wind-in-i-ghost-of-tsushima-i-)). God of War ran a full 3D GPU fluid sim ([GDC Vault](https://gdcvault.com/play/1025972/Wind-Simulation-in-God-of)). UE 5.7's experimental Dynamic Wind simulates plant bones on the GPU instead of offsetting vertices, so Nanite cluster bounds stay tight ([Epic docs](https://dev.epicgames.com/documentation/unreal-engine/dynamic-wind-for-nanite-foliage-in-unreal-engine)). Every AAA example shares one thing: **a single global wind field drives all wind consumers**. A Horizon Zero Dawn transcript describes a camera-attached wind field, but it comes from an unofficial fan mirror and is unverified.

For LOD, practice has moved from alpha-tested cards toward more opaque triangles and cluster or voxel far fields. Epic found Fortnite foliage "usually faster without masked materials". Masked materials add per-pixel barycentric cost and overdraw. Epic also added "Preserve Area" to stop simplified canopies thinning ([Epic Tech Blog](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)). UE 5.7's Nanite Foliage switches distant trees to voxel clusters of up to 128 bricks of 4×4×4 voxels ([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)). It headlined the Witcher 4 demo, which ran at 60 fps on a base PS5 ([CD PROJEKT RED](https://press.cdprojektred.com/news/1778/cd-projekt-red-and-epic-games-present-the-witcher-4-unreal-engine-5-tech-demo-at-the-state-of-unreal-2025)).

For an engine without Nanite, the proven stack has four parts:

- GPU-culled discrete LODs;
- dithered cross-fades that TAA resolves;
- hashed alpha testing for stable distant cutouts ([Wyman & McGuire, I3D 2017](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf));
- octahedral impostors at range, which Epic used in Fortnite and which have been ported to Godot, Three.js and Houdini ([shaderbits](https://shaderbits.com/blog/octahedral-impostors); [Godot port](https://github.com/SIsilicon/Godot-Octahedral-Impostors)).

Alpha-to-coverage needs MSAA, so it has little use in quat-3d's deferred TAA path. Horizon Forbidden West's deferred texturing for foliage did not survive the PC port ([GDC Vault](https://www.gdcvault.com/play/1027553/contactUs); [DF interview repost](https://gaminglatest.com/threads/horizon-forbidden-west-pc-tech-interview-with-nixxes-and-guerrilla-games.118617)). That is a warning against making it a priority.

Placement is where quat-3d's existing GPU terrain synthesis pays off. Horizon Zero Dawn's runtime GPU placement handled **500+ asset types and 100,000+ objects at about 250 µs** average GPU time. It evaluated artist rule graphs over 2D world maps stored at about 4 MB/km², with the pipeline instantiated 64 times, one layer each ([Guerrilla slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)). Far Cry 5's biome "recipes react to the physical features of the land" ([PlayStation Blog](https://blog.playstation.com/2018/03/22/the-procedural-world-generation-of-far-cry-5/)). UE 5.7 declared its PCG framework production-ready and about twice as fast as in 5.5 ([UE 5.7 preview](https://forums.unrealengine.com/t/unreal-engine-5-7-preview/2658958)).

The mapping onto quat-3d is direct. A per-layer `PlacementPass` reads the slope, curvature, erosion and wetness maps from R10.2 and writes instances for the cull and LOD pass. The existing CPU `Scatter` stays as the deterministic reference for tiles that need Jolt colliders or navigation data.

Leaf shading uses a two-sided foliage model. A subsurface transmission colour lets light pass through thin leaves, which Default Lit renders almost black ([Epic shading models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)). In quat-3d this becomes a GBuffer shading-model ID, a transmission term in the clustered lighting compute pass, and a `FoliageMaterial` Slang interface implementation.

Interaction should be a camera-centred, toroidally scrolled texture with a damped spring. Characters, Jolt bodies and explosions write velocity and displacement into it. Ghost of Yōtei extends the idea with a weapon "cut buffer" ([PlayStation Blog](https://blog.playstation.com/?p=411201)), and Jahrmann and Wimmer show per-blade compute physics ([I3D 2017](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf)).

Three correctness rules recur in all of this. First, the GBuffer, shadow and depth-prepass passes must apply identical wind offsets. Second, motion vectors must include previous-frame wind, so the wind field is double-buffered. Third, culling bounds must be padded by the maximum displacement.

One rule needs explicit clarification. CLAUDE.md says "never write skinning in the vertex shader". Procedural wind offsets are not skinning, and the rule should say so, so that agents do not route every leaf through compute skinning.

| Technique | Requirements / macOS | Cost | Fit | Priority |
|---|---|---|---|---|
| Global `WindFieldPass` (2D or small 3D clipmap: direction + gust noise + emitter SSBO), double-buffered | Core / yes | S | Sampled by grass, trees, particles, cloth, hair, ocean | **Quick win** |
| Crysis main + detail bending with per-instance phase from `Scatter` seed | Core / yes | S | Foliage VS in GBuffer and shadow passes | **Quick win** |
| Interaction/trample field with cut channel | Core / yes | S–M | Fed by Jolt characters, Lua, MCP impulses | **Quick win (with R9.3)** |
| Two-sided foliage shading model + back-face normal flip | Core / yes | S–M | GBuffer ID shared with hair cards and fur | **Quick win** |
| Coverage-preserving alpha mips + hashed alpha + masked depth prepass | Core / yes | S | Cooker + Slang helper | **Quick win** |
| Tsushima grass LOD (fewer, wider blades), view-space widening, camera-pitch tilt | Core / yes | S | Inside R9.3 ([GDC Vault](https://gdcvault.com/play/1027214/contactUs)) | **Quick win** |
| GPU instance cull + LOD + dithered cross-fade | Core 1.2 / yes | M | `FoliageCullPass` = R9.4/R6 | **Medium** |
| HZD-style GPU density placement + biome recipes | Core / yes | M–L | Reads R10.2 outputs; CPU `Scatter` reference | **Medium (high AI value)** |
| Octahedral/hemi-octahedral impostors writing the GBuffer, impostor shadows | Core / yes | M | Same indirect path | **Medium** |
| Pivot-Painter-style 2–4 level hierarchical wind | Core / yes | M | Cooker bakes pivot/parent/axis into BDA SSBO | **Medium** |
| Simulated wind (fluid or vorticles); skeletal wind for near trees | Core / yes | L | Replaces noise; reuses skinning compute | **Long-term** |
| Cluster/meshlet foliage + voxel far LOD | Mesh shader optional / partial | XL | After virtual geometry | **Long-term** |
| RT foliage via cluster BLAS + PTLAS | NV-only | XL | — | **Watch** |

## Weather, atmosphere and water share one WeatherState and one wind field

### Froxel fog and 2.5D clouds are the next sky upgrades

quat-3d's sky draws clouds as an FBM layer projected onto a plane (`assets/shaders/lib/sky.slang`) and already has transmittance, multi-scatter and sky-view LUTs. The proven next step is the 2015–2017 Nubis model:

- a weather map plus Perlin–Worley and Worley 3D noise;
- ray marching at reduced resolution;
- temporal reprojection to fill in the rest.

Guerrilla held this under a **2 ms PS4 budget** ([Guerrilla Nubis 2017](https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine)). Studio Gobo's optimisations cut march steps to 1/16 ([arXiv 1609.05344](https://arxiv.org/pdf/1609.05344)). Hillaire's Frostbite course pairs such a cloud layer with physically based sky rendering at 0.42 ms on Xbox One ([Frostbite 2016 course](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf)). His EGSR 2020 LUT atmosphere, which quat-3d's atmosphere already resembles, can change composition at runtime without heavy LUT updates ([EG Digital Library](https://diglib.eg.org/handle/10.1111/cgf14050)).

UE's cloud documentation names the cost levers. Use a cloud shadow map instead of secondary marches, which is "usually enough for clouds viewed from the ground". Use one multiple-scattering octave ([UE Volumetric Cloud](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine)).

Guerrilla later moved to voxel clouds with compressed-SDF marching and fluid-sim authoring, so players can fly through them ("Nubis Cubed", [Guerrilla](https://www.guerrilla-games.com/read/nubis-cubed)). That requires an authoring pipeline quat-3d does not have, so it is long-term.

Froxel volumetric fog is the industry standard. Wronski introduced it in AC4 with 160×90×64 or 160×90×128 RGBA16F volumes ([AC4 GDC 2014](https://bartwronski.com/wp-content/uploads/2014/03/ac4_gdc.pdf)). Hillaire unified it with participating-media physics in Frostbite ([EA](https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite)). UE reduces aliasing with "a heavy temporal reprojection filter with a different sub-voxel jitter per frame" ([UE Volumetric Fog](https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine)). Godot adds local `FogVolume` primitives that inject density into the grid ([Godot](https://godotengine.org/article/fog-volumes-arrive-in-godot-4)).

Reported costs are about 1–1.1 ms on PS4-class hardware, from secondary and dated sources. The froxel grid is the same frustum parameterisation as quat-3d's GPU clusters, which R10.1 already shipped. The scattering pass can therefore loop over the existing per-cluster light lists. Light shafts then come for free from shadowed in-scattering.

### Weather is a small global state with many consumers

Shipped weather is a set of loosely coupled systems driven by a few global scalars. For quat-3d that means a singleton POD `WeatherState` holding:

- cloud coverage and type;
- precipitation rate and type;
- wetness and snow cover;
- wind vector and gustiness;
- fog density;
- lightning probability.

A system blends it toward a target preset over time. Consumers read one UBO mirror: clouds, fog injection, atmosphere aerosols, rain and snow particles, the material wetness and snow terms, grass wind and the ocean spectrum. That gives Lua (G7) and MCP one control surface instead of dozens of disconnected parameters.

Rain comes from foundational work. Tatarchuk's ToyShop covered wet reflections and ripple simulation ([Advances 2006](https://advances.realtimerendering.com/s2006/Chapter3-Artist-Directable_Real-Time_Rain_Rendering_in_City_Environments.pdf)). AC4 added GPU rain, procedural ripples, a wet-material approximation and a top-down "world ambient occlusion" map that culls rain under cover ([Wronski, Digital Dragons 2014](https://bartwronski.com/2014/05/09/digital-dragons-2014-slides/)).

For quat-3d, apply wetness in a GBuffer-modify compute pass so every material gets it without shader permutations. The puddle mask should come from the terrain cavity and erosion data the engine already computes.

Snow deformation follows Batman: Arkham Origins and Rise of the Tomb Raider's "deferred deformation". A player-following heightmap is reconstructed in the snow pass rather than tessellated ([GPU Pro 7 chapter](https://www.gameenginegems.com/gemsdb/article.php?id=1314)). It can share the scrolling-clipmap infrastructure with the grass interaction field. No primary 2018–2026 source on lightning or on weather state machines was found. Those designs are engineering judgement.

### Water: FFT ocean with Jolt buoyancy, then erosion-fed rivers

The production ocean is a Tessendorf FFT heightfield with choppy displacement. It is driven by JONSWAP spectra, with directional spreading per Horvath ([DigiPro 2015](https://cdn.shopify.com/s/files/1/0134/1131/9867/files/EmpiricalDirectionalWaveSpectra_DigiPro2015_Jun20_2015_Optimized.pdf)), across several cascades of incommensurate sizes to hide the FFT's inherent periodicity.

Atlas shipped NVIDIA WaveWorks 2.0 with two JONSWAP spectra (local wind sea plus swell) and interactive wakes and explosions ([GPUOpen slides](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf)). Its server ran a CPU simulation for authoritative buoyancy ([NVIDIA blog](https://developer.nvidia.com/blog/three-things-you-need-to-know-about-wave-works-2-0)). Sea of Thieves layered a wave-peak subsurface mask over an FFT base ([SIGGRAPH 2018 talk](https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf)). The MIT-licensed Crest puts a dynamic-wave sim on top of FFT waves for interaction ([Crest docs](https://crest.readthedocs.io/en/4.23.0/user/waves.html)).

Shading follows UE's Single Layer Water: absorption and scattering coefficients plus refraction from a scene colour and depth copy ([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)).

For local water, Müller's heightfield columns handle ponds and rivers cheaply but cannot overturn ([Ten Minute Physics #20](https://matthias-research.github.io/pages/tenMinutePhysics/20-heightFieldWater.pdf)). quat-3d's erosion simulation already produces water flux that can become river flow maps.

Physics coupling is unusually easy. `JPH::Body::ApplyBuoyancyImpulse` takes a surface plane, buoyancy factor, drag and fluid velocity. The `BodyInterface` version wakes sleeping bodies only when it applies an impulse ([Jolt Body.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/Body.h)). Fitting one plane per body from a deterministic CPU evaluation of the dominant wave components satisfies CLAUDE.md's CPU-reference rule and mirrors Atlas.

| Technique | Requirements / macOS | Cost | Fit | Priority |
|---|---|---|---|---|
| `WeatherState` + Lua/MCP binding + wind texture to grass | Core / yes | S | One UBO, many consumers | **Quick win** |
| Cloud shadow map from existing 2D clouds | Core / yes | S | Terrain, fog and sun term | **Quick win** |
| Wetness/snow material layer + rain particles with top-down occlusion map | Core / yes | S–M | GBuffer-modify compute; needs R10.3 | **Quick win → medium** |
| Froxel fog + local fog volumes | Core / yes | M; ~1 ms PS4-class (secondary) | Shares R10.1 cluster slicing and light lists | **Medium (high value)** |
| 2.5D Nubis/Hillaire cloud layer, 1/4-res + temporal reprojection | Core / yes | M; ~2 ms PS4 budget | Reuses atmosphere LUTs, TAA history; Slang density interface | **Medium** |
| Puddles/ripples, snow deformation heightmap, lightning | Core / yes | M | Erosion cavity data; shared clipmap | **Medium** |
| FFT ocean (3–4 JONSWAP cascades) + single-layer water + Jolt buoyancy | Core / yes | M–L | Pure compute; CPU wave sampling for physics | **Medium** |
| Heightfield/SWE local water + erosion-derived river flow | Core / yes | M–L | Jolt bodies inject displacement | **Medium-long** |
| Voxel/SDF clouds, 3D wind fluid grid, caustics, wakes, spray | Core / yes | L–XL | Needs authoring tools | **Long-term** |

## Characters and dynamics reuse Jolt more than anything else

### Animation upgrades are pure maths with large visible payoff

Four animation techniques are cheap, deterministic and testable headlessly.

**Inertialization** comes from Bollo's work on Gears of War. It replaces cross-fades with a post-process that decays the source-to-target pose offset, so only one pose is evaluated during a transition ([GDC 2018](https://gdcvault.com/play/1025165/Inertialization-High-Performance-Animation-Transitions)). It slots directly into `AnimStateMachine`.

**Analytic two-bone IK with foot placement** uses Jolt raycasts, pelvis drop and foot-normal alignment. It runs after the blend tree and before world transforms. UE's full-body IK is position-based rather than classic FABRIK ([UE FBIK](https://dev.epicgames.com/documentation/unreal-engine/control-rig-full-body-ik-in-unreal-engine)).

**ACL** is the MIT-licensed animation codec that has been UE's default since 5.3 ([Frechette](https://nfrechette.github.io/2023/09/17/acl_in_ue)). It belongs in the cooker.

**Motion matching** builds on all three. It searches a feature database of trajectory and pose features every frame. Ubisoft introduced it at GDC 2016 ([GDC Vault](https://www.gdcvault.com/play/1022985/Motion-Matching-and-The-Road)), and it now ships in UE's Pose Search plugin ([UE docs](https://dev.epicgames.com/documentation/unreal-engine/motion-matching-in-unreal-engine?lang=en-US)). Learned Motion Matching replaces the database with small networks ([Ubisoft La Forge](https://www.ubisoft.com/en-us/studio/laforge/news/3NWqwcMU9lMfumgfPAEfka/learned-motion-matching)), but it needs a training pipeline.

For ragdolls, Jolt already provides `SkeletonMapper` and `Ragdoll::DriveToPoseUsingMotors`, which gained a position-plus-velocity drive in 5.6 ([Jolt release notes](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)). Death ragdolls and powered hit reactions are therefore integration work, not research.

### Hair and fur: cards and shells now, compute-rasterised strands later

Strand hair has shipped at 60 fps on consoles. Dragon Age: The Veilguard renders about **50,000 strands per character** on PS5, Series X and PC. It took more than four years of collaboration, and players can turn it off because of its memory and compute cost ([EA](https://www.ea.com/news/strand-hair-dragon-age-the-veilguard); [Microsoft Developer](https://developer.microsoft.com/en-us/games/articles/2024/12/bioware-dragon-age-the-veilguard-strand-hair-technology/)). The architecture has converged:

1. Skin or simulate strands in compute.
2. Software-rasterise one lane per segment into a visibility atlas.
3. Shade in strand space with a Marschner-derived BSDF plus approximate multiple scattering.
4. Self-shadow with deep opacity maps or voxels.
5. LOD down to cards.

Sources: Frostbite ([SIGGRAPH 2019](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf)); id Tech in Indiana Jones ([SIGGRAPH 2025](https://www.advances.realtimerendering.com/s2025/content/Strand%20Hair%20in%20IJGC%20-%20Final%20Slides%20(Post-Conference).pdf)); UE Groom ([Unreal blog](https://www.unrealengine.com/en-US/tech-blog/an-early-look-at-next-generation-real-time-hair-and-fur)); the 2026 I3D deferred strand rasteriser ([arXiv 2607.04230](https://arxiv.org/abs/2607.04230)). HairWorks is the counter-example. Its 64× tessellation into sub-pixel triangles crippled performance ([TechSpot](https://www.techspot.com/review/1006-the-witcher-3-benchmarks/page6.html)). TressFX 5 is now distributed only as an Unreal patch ([GPUOpen](https://gpuopen.com/learn/tressfx5/)).

The most important finding for simulation is that **Jolt 5.6 ships GPU Cosserat-rod hair with DX12, Vulkan and Metal compute backends**. Roots are skinned to the scalp, hair–hair interaction uses a velocity grid, and environment collision is limited to convex hulls and compounds ([Jolt release notes](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)). Jolt 5.7 fixed macOS jitter.

Whether its `ComputeSystemVK` can share quat-3d's device and VMA allocator is undocumented. That matters because CLAUDE.md requires VMA for every allocation. Answering this question is a medium-priority spike.

Shell fur remains a current technique: Capcom presented a modern shell-fur implementation at RE:2023 ([Capcom R&D](https://www.docswell.com/s/CAPCOM_RandD/KENVMJ-RE2023)). It fits quat-3d's instanced draws over compute-skinned vertices and writes alpha-tested results into the GBuffer, so no order-independent transparency is needed.

### Cloth and destruction map onto Jolt primitives the engine already has

Jolt's soft bodies provide what character cloth needs: compliance-based edges, dihedral bends, long-range tethers and skinning constraints ([Jolt-JNI soft-body guide](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/softbody.html)). Sub-stepped XPBD is the underlying formulation ([XPBD](https://matthias-research.github.io/pages/publications/XPBD.pdf); [Small Steps, SCA 2019](https://diglib.eg.org/handle/10.1145/3309486-3340247)). Jolt cloth should be the CPU reference, with a Slang XPBD cloth pass coming later for crowds. NvCloth is legacy and has no known Vulkan path.

Destruction has settled on **offline pre-fracture plus a runtime support graph**. Runtime mesh cutting has largely been abandoned. NVIDIA Blast is the clearest reference. Its chunks and bonds carry health, island detection on unbroken bonds spawns new actors, and an optional stress solver handles collapse ([Blast API](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pageintroduction.html); [stress solver](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pageextstress.html)). Unreal Chaos adds spatial "fields" that set strain, anchor and sleep behaviour ([Chaos Fields](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-fields-user-guide-in-unreal-engine)). Red Faction: Guerrilla had already replaced real-time CSG with pre-broken meshes ([GeoMod 2.0 wiki](https://www.redfactionwiki.com/wiki/GeoMod_2.0); fan source).

Networked destruction in shipped games is server-authoritative. The Finals runs destruction only on servers ([PC Gamer](https://www.pcgamer.com/i-almost-dont-believe-the-destructibility-in-this-fps-is-real/)). Battlefield 6 reworked its destruction-event replication for bandwidth and priority ([EA](https://www.ea.com/en/news/how-battlefield-6-redefined-destruction), search summary only).

On Jolt, each intact destructible is one body with a `StaticCompoundShape` of convex chunk hulls. When it breaks, each island becomes a new body ([Jolt docs](https://jrouwe.github.io/JoltPhysics)). quat-3d's `src/net/replication.cpp` should replicate broken-bond bitset deltas, because the fracture topology is shared content and only bit flips need to travel. Large debris travels as rigid-body snapshots. Cosmetic debris is seeded client-side. Teardown-style voxel destruction would need a different renderer and is out of scope unless it becomes a product goal ([TechRadar](https://techradar.com/gaming/consoles-pc/teardown-devs-top-priority-is-always-physics-the-game-can-come-later)).

### GPU particles unlock most VFX, and WBOIT makes them TAA-stable

R10.3 matches the canonical GPU particle architecture:

- dead and alive index lists;
- emit and simulate compute passes;
- indirect arguments written by compute;
- optional bitonic or radix sort;
- depth-buffer collision against GBuffer depth and normals;
- indirect instanced draws.

Sources: [Wicked Engine 2017](https://wickedengine.net/2017/11/gpu-based-particle-simulation); AMD "Holy Smoke" ([deck](https://www.slideshare.net/slideshow/holy-smoke-faster-particle-rendering-using-direct-compute-by-gareth-thomas/35656271)). Niagara calls depth collision "low-cost but also low-accuracy", and offers SDF and experimental ray-traced collision as upgrades ([Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/gpu-raytracing-collisions-in-niagara-for-unreal-engine)).

The transparent pass already binds the clustered light lists, so lighting particles costs almost nothing extra. Weighted Blended OIT needs only two extra float render targets. It "avoids sorting artifacts and popping for particle systems" ([McGuire & Bavoil, JCGT 2013](https://www.jcgt.org/published/0002/02/09/paper.pdf)), which makes it stable under TAA.

For smoke and fire, baked flipbooks remain the cheapest high-quality option. Next come baked sparse volume caches, then a local 64³–128³ stable-fluids grid ([GPU Gems 3 ch.30](https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids)). Position-based fluids are long-term ([Unified Particle Physics](https://matthias-research.github.io/pages/publications/flex.pdf)). EmberGen's sparse GPU solver shows the ceiling: about 2 billion active voxels on 48 GB ([CG Channel](https://www.cgchannel.com/2025/01/check-out-the-new-features-due-in-embergen-2-0)). It is a content-creation tool, not an in-game runtime.

| Technique | Requirements / macOS | Cost | Fit | Priority |
|---|---|---|---|---|
| Inertialization; two-bone IK + foot placement | None / yes | S | Animator; Jolt raycasts | **Quick win** |
| ACL compression | MIT dep / yes | S–M | Cooker + `Animator::update` | **Quick win** |
| Shell + fin fur | Instancing / yes | S | Compute-skinned verts; GBuffer ID | **Quick win** |
| Jolt ragdolls and powered hit reactions | Jolt ≥5.6 / yes | M | `SkeletonMapper` + inertialization | **Medium** |
| Jolt soft-body character cloth | Mesh cook for sim / yes | M | Skinning constraints; CPU reference | **Medium** |
| Motion matching node | Feature-DB cooker / yes | M | Needs inertialization, ACL, mocap | **Medium** |
| Hair cards + Marschner-approximation shading | Core / yes | M | Shading-model ID; dithered + TAA | **Medium** |
| Jolt GPU Cosserat hair evaluation | Jolt Vulkan/Metal compute / yes | M (spike) | Device/VMA sharing unknown | **Medium (evaluate)** |
| GPU particles + depth collision + soft particles + clustered lighting + WBOIT + flipbooks | Core / yes | M | R10.3; transparent pass | **Quick win / next** |
| Pre-fracture + support graph + Jolt compounds + breakable constraints + bond-delta replication + debris tiers | None / yes | M–L | Cooker Voronoi fracture; `DEBRIS` layer; instanced chunks | **Medium** |
| Compute-rasterised strand hair | int64 atomics / unverified | XL | Separate branch after lighting, before TAA | **Long-term** |
| GPU XPBD cloth; stress solver; SDF particle collision; smoke grid; PBF liquids | Core / yes | L–XL | Share scene SDF with GI | **Long-term** |
| Learned MM / DReCon; Teardown-style voxels; runtime mesh booleans | — | — | — | **Research / out of scope / avoid** |

## Neural rendering stays a watch list until Vulkan ratifies cooperative vectors

The research is mature. Neural texture compression (NTC), neural materials and neural radiance caching (NRC) all have SIGGRAPH papers:

- **Neural Texture Compression** ([NVIDIA](https://research.nvidia.com/labs/rtr/neural_texture_compression/)) has an open RTXNTC SDK. Its Vulkan 1.3 path uses `VK_NV_cooperative_vector` and has a slow DP4a fallback. Tom's Hardware reports VRAM reductions of **over 80%** ([RTXNTC](https://github.com/NVIDIA-RTX/RTXNTC); [Tom's Hardware](https://www.tomshardware.com/pc-components/gpus/benchmarking-nvidias-rtx-neural-texture-compression-tech-that-can-reduce-vram-usage-by-over-80-percent)).
- **Neural appearance models** are claimed to run more than an order of magnitude faster than layered non-neural materials ([NVIDIA Research](https://research.nvidia.com/publication/_real-time-neural-appearance-models)).
- **NRC** ([arXiv 2106.12372](https://arxiv.org/pdf/2106.12372)) has been adopted by at least one studio through cooperative vectors ([GameDev.net](https://gamedev.net/news/3317-cooperative-vectors-introduction/)).

The API layer is what is still unsettled. `VK_NV_cooperative_vector` is unratified. Only `VK_KHR_cooperative_matrix` is cross-vendor. On DirectX, Microsoft deprecated the original Cooperative Vector spec ([DirectX blog](https://devblogs.microsoft.com/directx/shader-model-6-9-and-the-future-of-cooperative-vector/)) and replaced it with DirectX Linear Algebra, which entered public preview in April 2026 targeting SM 6.10 with commitments from all four GPU vendors ([Microsoft GDC 2026](https://developer.microsoft.com/en-us/games/articles/2026/03/gdc-2026-evolving-directx-for-ml-era-on-windows/)). AMD's ML denoiser and radiance cache remain DX12-only. NVIDIA says Slang supports cooperative-vector intrinsics ([NVIDIA blog](https://developer.nvidia.com/blog/neural-rendering-in-nvidia-optix-using-cooperative-vectors)), which keeps the door open for quat-3d.

ML that ships through vendor SDKs (DLSS, FSR 4, XeSS) is ready today through the upscaler interface. The only low-risk neural experiment is **NTC "inference on load"**. It decompresses to ordinary BCn textures at load time, so the runtime renderer does not change, and it saves disk and download size even on the DP4a fallback. Neural materials, NTC inference on sample, and NRC should wait for a KHR or EXT counterpart to DirectX Linear Algebra.

## Adoption roadmap: quick wins, medium and long-term by track

### Eight shared dependencies order the whole plan

Most of the techniques above depend on a few shared substrates. Building each substrate once, as a named render-graph resource or ECS singleton, is what keeps the techniques cheap.

| Shared substrate | Consumers | Build it in |
|---|---|---|
| **Hi-Z depth pyramid** | R6/R9.4 occlusion culling, foliage cull, GTAO/SSILVB, SSR, contact shadows, screen-space GI | R6 (first) |
| **Complete velocity buffer + jitter contract** | TAA, FSR/DLSS/XeSS, foliage wind (previous-frame offsets), skinned meshes (previous positions), particles (reactive mask), cloud and fog reprojection, hair masks | New upscaler item under R8 |
| **Global wind field (from `WeatherState`)** | Grass, tree bending, cloth, Jolt hair, particles, ocean spectrum, cloud drift, Jolt drag | R9.3 / weather item |
| **4-cascade CSM with GPU depth bounds** | Foliage, terrain, froxel in-scattering, particle shadowing, all GI injection | R8 (before any GI) |
| **One radiance-cache interface with pluggable tracers** | Probe volume tiers 0–1, SDF tracer, ray-query tracer, SSR fallback, ReSTIR candidates | New GI item (data structure medium, tracers long) |
| **Global scene SDF** | SDF GI tracing, SDF particle collision, SDF reflections, possibly AO | Long-term, with GI tier 1 |
| **Camera-centred toroidal clipmap textures** | Grass interaction and cut buffer, snow and mud deformation, local water sim, wind clipmap, rain occlusion map | R9.3 (first user) |
| **GBuffer shading-model ID** | Two-sided foliage, hair cards, shell fur, wetness/snow modify, water | R8 or R9.3 |

Four ordering rules follow. Shadows and AO come before GI. Hi-Z comes before SSR and occlusion culling. Velocity comes before any upscaler or any foliage wind that must survive TAA. And R10.2's terrain outputs (slope, curvature, erosion flux, wetness) feed three otherwise separate features: GPU placement, puddle masks and river flow maps.

Every GPU path that affects gameplay (placement colliders, buoyancy heights, wind used for physics) needs a CPU reference implementation with a GoogleTest. Those tests should run on lavapipe, which has no ray tracing.

### How each feature should reach AI tools

The MCP surface should follow one principle: **expose intent-level data, not shader knobs**, and verify the result with screenshots. A8's planned settings schema (ranges plus units, saved with the scene) is the right home for every quality parameter: GTAO radius, cascade splits, GI tier, upscaler mode, fog density. The other categories need their own tools:

- **World-building data** should be first-class remote commands, each validated through `json_fields.hpp` with precise errors so models can self-correct:
  - `set_weather {preset, transition_s}`, plus a `WeatherState` read-back;
  - wind emitters;
  - local fog volumes as ECS components;
  - biome and layer recipes (species, density expression, slope/height/wetness ranges, clustering, exclusion radius);
  - water bodies;
  - destruction fields (strain, anchor, sleep volumes);
  - particle emitter descriptors.
- **Debugging and inspection** belongs in A5:
  - GPU pass timings for budget checks;
  - probe and SDF debug views;
  - motion-matching cost breakdowns;
  - the Hi-Z and velocity visualisers;
  - screenshot capture (A3) of each, so a model can see what it changed.
- **Material work** goes through A6. Models author Slang implementations of the material interface (`FoliageMaterial`, water, hair) with compiler diagnostics returned to them.

This turns "make the north valley a wet birch forest at dusk in light rain" into a sequence of declarative, testable tool calls.

### Phased plan

| Phase | Track | Items | Key dependencies | MCP exposure |
|---|---|---|---|---|
| **Quick wins** | R6 | Hi-Z pyramid | — | Hi-Z debug view (A5) |
| | R8 | XeGTAO port + bent normals → SSILVB; contact shadows; 4-cascade CSM with GPU depth bounds, texel snapping, PCF/PCSS | Hi-Z; normals | A8 settings; screenshot verification |
| | R8 (proposed upscaler item) | Velocity buffer (skinned previous positions, instanced, wind, particles), reactive mask, upscaler node, FSR 2/3.1 | — | Upscaler mode and scale in A8 |
| | R9.3 | Compute grass with wind field, interaction/cut field, Tsushima LOD, camera-pitch tilt | Wind field, clipmap infra | Wind emitters and impulses as remote commands |
| | R9 (foliage) | Main + detail bending, two-sided foliage shading ID, hashed alpha + coverage-preserving mips, masked depth prepass | Velocity; shading-model ID | Foliage material params via A6/A8 |
| | R10.3 | GPU particles, depth collision, soft particles, clustered lighting, WBOIT, flipbooks | GBuffer depth/normal | Emitter descriptors as data |
| | Weather (proposed R/G item) | `WeatherState`, wind texture, cloud shadow map, wetness/snow layer, rain + occlusion map | Wind field; R10.3 | `set_weather`, Lua binding (G7) |
| | R1/R3 cooker | KTX2/Basis, meshopt codecs and quantisation | — | — |
| | G (proposed animation item) | Inertialization, two-bone foot IK, ACL, shell fur | Jolt raycasts | Clip/transition inspection (A5) |
| **Medium** | R6/R9.4/R5 | GPU instance cull + LOD + dithered cross-fade, indirect-count draws, meshlets, guarded mesh-shader path | Hi-Z, velocity | Cull statistics and timings (A5) |
| | R9/R10.2 | GPU density placement from terrain maps, CPU `Scatter` reference, async collider tiles; octahedral impostors; hierarchical pivot wind | R10.2 outputs; cull pass | **Biome recipe authoring** (highest AI value) |
| | R8 | Stochastic SSR; probe volume data structure (SH or octahedral + depth, relocation, classification) as GI tier 0 | Hi-Z; CSM | GI tier and probe views |
| | R (atmosphere) | Froxel fog + local fog volumes; 2.5D cloud layer with temporal upsample | R10.1 clusters; CSM; TAA history | Fog volumes and cloud coverage/type |
| | R/G (water) | FFT ocean + single-layer water + Jolt buoyancy; heightfield/SWE local water and erosion flow maps | Wind/WeatherState; froxel fog for underwater | Water body definitions |
| | R8 (RT tier, Win/Linux) | Ray-query tracer, BLAS/TLAS in graph with skinned refit, RT sun shadows and AO, SIGMA/ReBLUR-class denoise | Radiance-cache interface | RT toggles in A8 |
| | R8 plugins | DLSS SR, XeSS-SR, FSR 4.1 | Upscaler node | Mode list reflects detected vendor |
| | G (physics/anim) | Jolt ragdolls and powered hit reactions, Jolt cloth, motion matching, hair cards, Jolt GPU hair spike | Inertialization; Jolt ≥5.6 | Ragdoll/cloth params |
| | G4 + physics | Pre-fracture cooker, support graph, Jolt compounds, breakable constraints, bond-delta replication, debris tiers and `DEBRIS` layer | R10.3 for dust; instancing | **Destruction fields** and break events |
| **Long-term** | R (GI) | Global sparse SDF + DDGI tracing (Brixelizer-style), screen-probe gather + world cache; ReSTIR DI/GI and path tracing | CSM, probe interface; RT tier | GI tier, debug views |
| | R5 + geometry | Visibility buffer + material binning, cluster DAG LOD, software raster, streaming, software virtual texturing, virtual shadow maps | GPU-driven pipeline; int64 atomics (feature-checked) | Streaming and residency stats |
| | R (nature) | Simulated wind (fluid or vorticles), skeletal near-tree wind, voxel/cluster foliage far field, Nubis-Cubed voxel clouds, ocean caustics/wakes/spray | Wind field; virtual geometry | Wind and cloud authoring |
| | R/G (characters, VFX) | Compute-rasterised strand hair, GPU XPBD cloth, stress solver, SDF particle collision, local smoke grid, PBF liquids, frame generation | Scene SDF; latency stack | Emitter and sim descriptors |
| **Watch / avoid** | — | NV cluster AS and PTLAS for RT foliage; neural materials, NRC, NTC inference on sample (until a KHR coop-vector extension exists); voxel cone tracing; runtime mesh booleans; tessellated hair; Teardown-style voxel destruction; HFW-style deferred texturing | — | — |

Two process notes apply. First, CLAUDE.md requires scope changes to be written into `ROADMAP.md` before work starts. The "proposed" items above (an upscaler item, a weather/atmosphere item, a GI item, an animation item and a destruction item) should be added as IDs first. Second, R10.1 GPU clustered culling has already shipped. The froxel-fog dependency that the research notes list as "planned" is therefore already satisfied.

## How far to trust these sources

The research sandbox's DNS policy blocked many of the hosts that hold the canonical sources:

| Host type | Blocked hosts |
|---|---|
| Talk and paper archives | `gdcvault.com`, `advances.realtimerendering.com`, `jcgt.org` |
| Engine and SDK documentation | `dev.epicgames.com`, `gpuopen.com`, `developer.nvidia.com`, `jrouwe.github.io`, `docs.mesa3d.org` |
| Studio sites | `guerrilla-games.com`, `www.ea.com` |
| Personal blogs | `shaderbits.com`, `zeux.io`, `jms55.github.io`, `sebh.github.io` |
| Industry news | `80.lv` |

As a result, the notes tag claims at four levels of confidence:

1. **Verified.** Read directly this session: XeGTAO, the NRD README, the MoltenVK changelog, Jolt release notes, the RTXNTC repository.
2. **Verified via search summary.** Read from a search extract of the primary page: Brixelizer GI, GI-1.0 timings, FSR SDK 2.4, KosmicKrisp conformance, the HZD placement numbers.
3. **Canonical, unverified.** The link is correct but the details come from background knowledge: DDGI internals and timings, Lumen's architecture, virtual-shadow-map page sizes, stochastic SSR, PCSS, voxel cone tracing, the Call of Duty and Division probe talks, Crest's LOD-data design, Lagarde's wet-surface series.
4. **Secondary or low confidence:**
   - the HZD camera-attached wind field (fan-hosted transcript);
   - the Tsushima figure of about 83k grass blades at about 2.5 ms;
   - UE froxel fog at about 1 ms on PS4 and 3 ms on a GTX 970 (old 80.lv cheat sheet);
   - the AC4 fog cost of about 1.1 ms (forum write-up);
   - The Finals using Chaos Voronoi fracture;
   - the Battlefield 6 networking description;
   - Godot HDDAGI timings.

The report cites tier 3 and tier 4 claims only where they shape direction, not decisions about budgets.

The notes also contain known conflicts and unknowns:

- MoltenVK 1.4.2's date differs between Wikipedia and the changelog.
- Press and release notes disagree on whether DDGI was part of RTXGI 2.0.
- Four facts that matter for adoption are still unknown:
  - whether Brixelizer's Vulkan path needs 64-bit atomics that MoltenVK lacks;
  - whether Jolt's GPU hair can share quat-3d's device and VMA allocator;
  - the exact Jolt API names for reading constraint impulses (needed for breakable joints);
  - whether DLSS frame generation works on Vulkan in Streamline today.
- **No per-technique timing on Apple Silicon or MoltenVK was found for anything.**
- Every "S/M/L/XL" cost in this report is an engineering estimate, not a measurement.
- Published millisecond figures come from PS4-era consoles or high-end NVIDIA and AMD desktop GPUs.

Allowing the blocked hosts and re-reading the GDC, Advances and JCGT primary sources would close most gaps. The first quat-3d benchmarks should be on the M-series Mac running KosmicKrisp, because it is the tightest platform.

## Conclusion

Two lessons from the 2018–2026 literature should shape quat-3d's plan. First, the frontier no longer decides what ships. Platforms decide. Lumen, Brixelizer GI, Nanite's fallback meshes and UE's own wind rework each keep a single data model and swap only the tracer, rasteriser or simulation backend underneath it. quat-3d's mix of Vulkan, MoltenVK and KosmicKrisp, with Metal planned, makes that pattern mandatory. Built that way, the compute tier that macOS needs is also the tier CI can test, and the RT and mesh-shader tiers become accelerators rather than forks.

Second, quat-3d's unusual assets are its GPU terrain and erosion data and its MCP bridge. Together they offer a differentiator no catalogue technique provides on its own. Biome recipes, a single `WeatherState` and declarative fields for wind, fog and destruction turn decades of AAA rendering practice into a vocabulary an AI agent can compose and check with screenshots. The highest-leverage work in the next cycles is therefore modest code that is also the foundation for everything later: Hi-Z, velocity, cascades, the wind field and GPU culling. Each makes several catalogue techniques cheap and makes the world more controllable through MCP.
