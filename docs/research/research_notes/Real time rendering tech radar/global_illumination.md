# Real-time Global Illumination, Shadows, Reflections and Ambient Occlusion (2018–2026): Tech Radar for quat-3d

Scope note on sourcing. Each claim below has an inline link. Claims marked **[verified]** were read in this session from the linked page, or from a search result that quoted that page. Claims marked **[canonical, unverified]** link to the canonical primary source, but its host was blocked by this session's network policy, so the detail comes from background knowledge and needs a check before anyone relies on it. The blocked hosts were jcgt.org, advances.realtimerendering.com, dev.epicgames.com and docs.mesa3d.org (DNS refused).

Engine baseline, checked in the repo this session:
- Rendering: deferred GBuffer, GPU clustered light culling, a single directional shadow map (`src/renderer/shadow_pass.cpp`, 143 lines), TAA, IBL (static and dynamic probe), atmosphere, terrain and vegetation.
- Roadmap: `ROADMAP.md` item **R8 "Image quality"** already lists "GTAO, screen-space reflections, contact shadows, optional ray-traced shadows/AO". **R6** lists Hi-Z occlusion culling.
- Shaders: no AO, SSR, GI or ray-query shader exists in `assets/shaders/`.

---

## 1. Probe-based GI (DDGI / RTXGI, precomputed and relit probes, irradiance volumes / SH, adaptive placement)

### Takeaway
DDGI is the most portable dynamic diffuse-GI technique in the catalogue. It is a world-space grid of probes that store octahedral irradiance plus depth moments with Chebyshev visibility, and it is updated by a few hundred rays per probe per frame. Its only hard RT dependency is the probe ray trace, and that trace can be swapped for SDF, voxel or screen tracing. NVIDIA has since moved its current SDK (RTXGI 2.x) to path-tracing radiance caches (NRC, SHaRC) and split DDGI into a separate v1.x repo. Baked or relit probe volumes remain the no-RT baseline that shipping titles use.

### Cited Findings
- **DDGI paper.** Majercik, Guertin, Nowrouzezahrai, McGuire, "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields", JCGT 8(2), 2019. Probes on a 3D grid store octahedral-mapped irradiance and mean/mean² distance. A Chebyshev (VSM-style) visibility test prevents light leaking. Each frame traces N rays per probe (typically 100–300) and blends into the textures with hysteresis. — [JCGT 2019](https://jcgt.org/published/0008/02/01/) **[canonical, unverified]**
- **RTXGI repo split.** The main RTXGI README describes v2.0 as providing "an example integration (DX12 and Vulkan) of two state-of-the-art radiance caching techniques for path tracing": the experimental AI-based **Neural Radiance Cache (NRC)** and the **Spatially Hashed Radiance Cache (SHaRC)**. It says the DDGI algorithm "lives in v1.x … in a separate repo" at github.com/NVIDIAGameWorks/RTXGI-DDGI. — [RTXGI project listing](https://awesome.ecosyste.ms/projects/github.com%2FNVIDIA-RTX%2FRTXGI), [NVIDIA RTXGI getting started](https://developer.nvidia.com/rtxgi-getting-started) **[verified via search summary]**
- **Conflicting coverage of RTXGI 2.0.** Press coverage at the 2.0 launch listed DDGI as one of three headline features, but the official release notes list only NRC and SHaRC as additions. The two accounts conflict. — [Wccftech](https://wccftech.com/nvidia-rtxgi-2-0-available-next-frontier-ray-traced-visuals-neural-radiance-cache-spatial-hash-radiance-cache-dynamic-diffuse-global-illumination/) **[verified via search summary]**
- **SHaRC requirements.** SHaRC is described as running on "any DirectX or Vulkan ray-tracing-capable GPU". NRC is "currently available in experimental mode". — [The FPS Review](https://www.thefpsreview.com/?p=116574) **[verified via search summary]**
- **Neural radiance caching paper.** Müller et al., "Real-time Neural Radiance Caching for Path Tracing", SIGGRAPH 2021. — [arXiv 2106.12372](https://arxiv.org/pdf/2106.12372) **[verified link]**
- **Brixelizer GI caches.** AMD's Brixelizer GI uses "screen probes backed by a world-space radiance and irradiance cache". This is the same probe-cache pattern as DDGI and Lumen, but traced against SDFs instead of triangles (see §2). — [GDC 2024 Brixelizer GI](https://gpuopen.com/download/GDC2024_GI_with_AMD_FidelityFX_Brixelizer.pdf) **[verified via search summary]**
- **Precomputed and relit GI in shipped games** **[canonical, unverified]**:
  - *Tom Clancy's The Division* (Massive, GDC 2016) used precomputed "probe" radiance transfer that is relit at runtime for dynamic time-of-day.
  - Activision's Call of Duty work: "Precomputed Lighting in Call of Duty: Infinite Warfare" (Iwanicki/Sloan, SIGGRAPH 2017 Advances) and the "Large-Scale Global Illumination at Activision" line of talks.
  - Enlighten (Geomerics/Silicon Studio) is the commercial relightable precomputed-radiosity middleware.
  - Sources: [SIGGRAPH Advances course index](https://advances.realtimerendering.com/), [Activision Research](https://research.activision.com/)
- **Shipped DDGI-style titles.** Probe-grid GI with RT updates is used in titles such as *Metro Exodus Enhanced Edition*, which uses "infinite-bounce" GI through a probe/volume cache. — **[canonical, unverified; no primary source fetched]**

### Inferences
- **Why DDGI fits quat-3d best as a first dynamic GI.**
  - It is world-space, so it is stable under TAA and has no screen-space disocclusion.
  - The update is a fixed-size compute workload (probes × rays), which matches "GPU compute first".
  - Sampling it from `lighting.slang` is a trilinear 8-probe fetch. It slots into the existing deferred lighting compute pass next to IBL.
- **Making DDGI work without hardware RT.** The probe-ray "trace" step is the only part that needs RT. On MoltenVK, KosmicKrisp and older GPUs it can be replaced by tracing against:
  - a coarse global SDF or voxel clipmap (§2); or
  - rasterised probe cubemaps (the "cheap DDGI" variant).

  This gives one GI integration with two tracers.
- **Adaptive placement.** Probe relocation (moving probes out of geometry) and probe classification (disabling probes in empty space or inside walls) are standard DDGI-v1.x features. They matter for terrain, where large uniform volumes waste probes. Cascaded or scrolling volumes around the camera are the standard answer for open worlds.
- **Precomputed fallback for content.** A static baked SH/irradiance probe volume (L1 or L2 SH per probe), relit only by sky intensity, is the cheapest tier. It is a reasonable "Tier 0" for integrated GPUs.

### Gaps
- No DDGI timing numbers could be verified, because the JCGT host was blocked. The paper reports sub-millisecond-to-low-ms update costs on RTX 2080-class GPUs, but the exact figures need checking.
- The current status of the RTXGI-DDGI v1.x repo (archived or maintained) was not verified.
- No primary source was fetched for The Division or the CoD probe talks. The talk titles listed are from background knowledge.

---

## 2. Software / hybrid RT GI (Lumen, global SDF tracing, surface cache, AMD GI-1.0 / Brixelizer GI, Godot SDFGI / HDDAGI, voxel cone tracing)

### Takeaway
The dominant non-hardware-RT approach since 2021 has three parts:
1. a cheap scene representation: mesh SDFs, a global SDF clipmap, sparse SDF bricks or voxels;
2. a lighting cache on that representation: surface cache cards, or world-space radiance/irradiance hash caches;
3. a screen-space probe final gather with temporal and spatial reuse.

Lumen (UE5), AMD GI-1.0 / Brixelizer GI and Godot SDFGI / HDDAGI all follow this pattern. AMD's Brixelizer GI is the only one available as an MIT-licensed, compute-only Vulkan library, which makes it the most directly adoptable reference for quat-3d.

### Cited Findings
- **Lumen at SIGGRAPH 2022.** "Lumen: Real-time Global Illumination in Unreal Engine 5" (Wright, Narkowicz, Kelly), SIGGRAPH 2022 Advances. Its final gather uses adaptive downsampling with spatial and temporal reuse. — [SIGGRAPH 2022 Advances slides](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf) **[verified title/authors via search; slides host blocked]**
- **Lumen architecture** **[canonical, unverified; Epic docs host blocked]**:
  - Hybrid tracing: screen traces first, then mesh SDF traces near the camera, then global SDF traces far away, or hardware RT where available.
  - Hits are shaded from a **surface cache**: per-mesh "cards" with captured material and lighting.
  - Final gather uses octahedral screen-space probes placed adaptively on the GBuffer, plus a world-space radiance cache for distant lighting.
  - Reflections reuse the same tracing.
  - Source: [Lumen technical details (Epic docs)](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)
- **Lumen is UE5's default.** Epic describes Lumen as "Unreal Engine 5's fully dynamic global illumination and reflections system, which is enabled out of the box". — [Epic tech blog](https://www.unrealengine.com/en-US/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen) **[verified via search summary]**
- **Lumen budget in Fortnite.** For Fortnite Chapter 4 at 60 fps, Epic "were only going to have four milliseconds for dynamic global illumination and reflections combined". Lumen GI "downsamples heavily in order to fit into a four millisecond budget". Software RT "was required to hit the 60 fps budget due to its lower tracing costs" versus the 30 fps Matrix Awakens demo. — [Epic: Lumen in Fortnite Chapter 4](https://www.unrealengine.com/en-US/tech-blog/lumen-brings-real-time-global-illumination-to-fortnite-battle-royale-chapter-4) **[verified via search summary]**
- **AMD GI-1.0 paper.** Boissé et al., "GI-1.0: A Fast and Scalable Two-Level Radiance Caching Scheme for Real-Time Global Illumination". It is AMD Technical Report 22-10-9831 (Oct 2022), arXiv 2310.19855, and a SIGGRAPH Asia 2023 Technical Communication.
  - Its two cache levels are a **screen cache** (probes on visible surfaces) and a **world cache** (a hash-based spatial structure).
  - The implementation is in AMD's open **Capsaicin** framework.
  - Reported cost is **1.932–3.124 ms on a Radeon RX 6900 XT**.
  - Sources: [arXiv](https://ar5iv.labs.arxiv.org/html/2310.19855), [GPUOpen PDF](https://gpuopen.com/download/GPUOpen2022_GI1_0.pdf), [Capsaicin](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin), [project page](https://takahiroharada.github.io/gi-1.0) **[verified via search summary]**
- **Note on GI-1.0 tracing.** GI-1.0 as published in Capsaicin uses hardware ray tracing for its traces (DXR inline). **[canonical, unverified]**
- **AMD FidelityFX Brixelizer GI** **[verified]**:
  - "Compute-based real-time dynamic global illumination … built upon the sparse distance fields from AMD FidelityFX Brixelizer", "implemented in compute without hardware-accelerated ray-tracing".
  - It outputs denoised indirect diffuse **and** specular. It supports **DirectX 12 and Vulkan** under the **MIT license**. The latest listed version is v1.1.4, shipped with FidelityFX SDK 1.1.
  - Sources: [GPUOpen Brixelizer/GI](https://gpuopen.com/fidelityfx-brixelizer/), [Wccftech SDK 1.1](https://wccftech.com/amd-fidelityfx-brixelizer-gi-is-a-compute-based-dynamic-global-illumination-solution-available-now-in-sdk-1-1/)
- **Brixelizer internals** **[verified via search summary]**:
  - It generates sparse distance fields for triangle geometry in real time.
  - Each cascade is 64×64×64 voxels, and occupied voxels hold local distance-field "bricks".
  - Brixelizer GI is described as "a simplified implementation of AMD GI-1.0" (screen probes plus a world-space radiance and irradiance cache).
  - Caveats from AMD: "inaccuracies are expected due to the lower-frequency nature of the SDF scene representation", and a larger ray bias may be needed for depth-buffer/SDF mismatch.
  - Sources: [Introducing Brixelizer](https://gpuopen.com/learn/introducing-amd-fidelityfx-brixelizer/), [GDC 2024](https://gpuopen.com/gdc-presentations/2024/GDC2024_GI_with_AMD_FidelityFX_Brixelizer.pdf)
- **Godot SDFGI.** Godot 4 ships SDFGI: cascaded SDF plus probe-based, real-time GI with no bake and no RT hardware. — [Godot blog](https://godotengine.org/article/godot-40-gets-sdf-based-real-time-global-illumination/), [Godot docs](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_sdfgi.html) **[verified via search summary]**
- **HDDAGI, Godot's planned successor** **[secondary sources only]**:
  - It is the successor to SDFGI. It updates cascades locally so it stays fast while moving, leaks less light and uses less memory.
  - Linietsky ("reduz") reported early numbers of a probe-trace pass dropping from about 2.4 ms to about 0.9 ms on a GTX 1650.
  - The PR ("Upgrade SDFGI to HDDAGI") was revived in 2026 and flagged as compatibility-breaking.
  - SDFGI's SH was reported to be "buggy and not energy conserving".
  - Sources: [GameFromScratch](https://gamefromscratch.com/godot-new-global-illumination-system-hddagi/), [Godot proposals #10194](https://github.com/godotengine/godot-proposals/discussions/10194)
- **Voxel cone tracing (background).** Crassin et al., "Interactive Indirect Illumination Using Voxel Cone Tracing" (2011). It was the basis of SVOGI in CryEngine and of Godot's VoxelGI. It is known for leaking and high voxelisation cost, and the industry has largely moved to SDF or probe caches. — **[canonical, unverified; no source fetched]**

### Inferences
- **Brixelizer GI as the reference design.** It is Vulkan, MIT-licensed and compute-only, so it is the closest drop-in reference for quat-3d's no-RT tier. It also has the same two-level cache that scales up to the hardware-RT tier when the tracer is swapped. The main porting cost is HLSL→Slang, which is relatively mechanical, plus wiring Brixelizer's SDF build into the render graph as compute passes.
- **MoltenVK / KosmicKrisp compatibility.** These SDF/voxel approaches use only compute plus 3D textures and atomics, so they should run on MoltenVK and KosmicKrisp. This is the key reason to prefer them over a hardware-RT-only design given the macOS target.
- **Building a Lumen-class system in-house** is a long-term, multi-person-year effort: mesh SDF generation, surface cache cards, screen probes and radiance cache. Brixelizer-GI-style (global sparse SDF plus a probe cache) is the more tractable "proper" version for a small team.
- **Terrain is a special case.** A heightfield can be ray-marched analytically, and quat-3d already has GPU terrain tiles. It should be traced directly rather than voxelised into the SDF, which avoids SDF memory blow-up over large terrain.

### Gaps
- No verified per-pass Lumen timings on PS5 or PC, because the slides host was blocked.
- No verified Brixelizer GI ms numbers on reference GPUs. The GDC 2024 deck likely has them but could not be read.
- Whether Brixelizer's Vulkan backend requires extensions unavailable on MoltenVK (for example 64-bit atomics or specific subgroup ops) is unverified.

---

## 3. Hardware ray tracing (VK_KHR_ray_query / pipeline): ReSTIR DI/GI, RTXDI, path-traced games, denoisers, hybrid RT shadows/reflections

### Takeaway
On RT-capable desktop GPUs the 2020s state of the art has four parts:
- (a) ReSTIR-family reservoir resampling for many-light direct lighting (RTXDI) and indirect lighting (ReSTIR GI);
- (b) radiance caching (SHaRC, NRC) to terminate paths early;
- (c) dedicated denoisers (NRD ReBLUR/ReLAX/SIGMA, SVGF lineage, AMD FidelityFX Denoiser), or ML reconstruction such as DLSS Ray Reconstruction;
- (d) full path tracing in flagship PC titles such as Cyberpunk 2077 RT Overdrive and Alan Wake 2.

For quat-3d, ray-query RT shadows/AO/reflections plus NRD-class denoising are the realistic first step. ReSTIR and path tracing are a long-term tier.

### Cited Findings
- **ReSTIR GI.** Ouyang, Liu, Kettunen, Pharr, Pantaleoni, "ReSTIR GI: Path Resampling for Real-Time Path Tracing", HPG 2021 / CGF 40(8).
  - It extends spatiotemporal ReSTIR from direct to indirect light by reusing secondary-hit samples across pixels and frames.
  - It reports **MSE improvements of 9.3×–166×** at 1 path per pixel, and "in conjunction with a denoiser … high-quality path traced GI at real-time frame rates".
  - Follow-up work notes that as a screen-space method it "can only reuse the path at the first bounce". World-space variants address this.
  - Sources: [NVIDIA Research](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing), [EG Digital Library PDF](https://diglib.eg.org/bitstream/handle/10.1111/cgf14378/v40i8pp017-029.pdf) **[verified via search summary]**
- **ReSTIR DI.** Bitterli, Wyman, Pharr, Shirley, Lefohn, Jarosz, "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting", SIGGRAPH 2020. It renders millions of dynamic lights with a few shadow rays per pixel. Productised as **RTXDI**. — [NVIDIA Research](https://research.nvidia.com/publication/2020-07_spatiotemporal-reservoir-resampling-real-time-ray-tracing-dynamic-direct) **[canonical, unverified]**
- **NVIDIA Real-time Denoisers (NRD)** **[verified from README fetched this session]**:
  - Denoisers: **REBLUR** (recurrent blur), **RELAX** (A-trous based, "designed for RTXDI") and **SIGMA** (shadow-only).
  - Cost on **RTX 4080 at 1440p native**:
    - REBLUR_DIFFUSE_SPECULAR: 2.55 ms (3.40 ms in SH mode)
    - RELAX_DIFFUSE_SPECULAR: 3.25 ms (4.80 ms SH)
    - SIGMA_SHADOW: 0.40 ms
    - SIGMA_SHADOW_TRANSLUCENCY: 0.45 ms
  - Integrates into "any D3D12, Vulkan or D3D11 engine". Vulkan needs SPIR-V binding offsets from `GetLibraryDesc`.
  - Shaders are HLSL (`NRD.hlsli`). A `REBLUR_PERFORMANCE_MODE` targets consoles.
  - When using RIS/ReSTIR, the hitT of the chosen sample "must be probabilistically filtered … through the lens of the actual BRDF lobes".
  - Source: [NRD README](https://github.com/NVIDIA-RTX/NRD)
- **Cyberpunk 2077 RT Overdrive.**
  - GDC 2023 talk: "Cyberpunk 2077 RT: Overdrive – Bringing Path Tracing into Night City".
  - The mode shipped as a technology preview on 11 April 2023. NVIDIA recommended RTX 40-series plus DLSS 3.
  - It comprises RTX Direct Illumination (ReSTIR), multi-bounce indirect and full-resolution RT reflections.
  - A GDC 2024 follow-up covers ReSTIR GI, radiance caching and DLSS Ray Reconstruction in the Ultimate Edition.
  - Sources: [NVIDIA blog](https://blogs.nvidia.com/blog/2023/03/22/cyberpunk-2077-path-traced-visuals-gdc/), [NVIDIA GeForce news](https://www.nvidia.com/en-us/geforce/news/cyberpunk-2077-ray-tracing-overdrive-update-launches-april-11.md), [GDC24 session](https://www.nvidia.cn/on-demand/session/gdc24-gdc1002) **[verified via search summary]**
- **Cyberpunk path tracing limits.** CDPR's Q&A noted that volumetrics and forward-rendered elements stay outside the path-tracing domain. — [Wccftech Q&A](https://wccftech.com/cyberpunk-2077-path-tracing-q-a-plenty-room-improve/amp/) **[verified via search summary]**
- **Ray query vs pipeline.** `VK_KHR_ray_query` traces rays from any shader stage (graphics, compute, RT), is separate from `VK_KHR_ray_tracing_pipeline`, and can be combined with it. — [Khronos VK_KHR_ray_query](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_ray_query.html), [Vulkan Guide: Ray Tracing](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html) **[verified]**
- **Hybrid RT in shipped games** **[canonical, unverified; no primary source fetched this session]**:
  - *Battlefield V* (DICE/SEED, 2018): RT reflections.
  - *Metro Exodus* (4A, 2019): RT GI/AO, with an all-RT-lighting Enhanced Edition in 2021.
  - *Control* (Remedy, 2019): RT reflections, contact shadows and indirect diffuse.
  - *Alan Wake 2* (Remedy, 2023): path tracing.
  - SVGF: Schied et al., "Spatiotemporal Variance-Guided Filtering", HPG 2017.
  - AMD FidelityFX Denoiser: shadows and reflections.

### Inferences
- **Start with ray queries in compute.** quat-3d's compute-first render graph fits `VK_KHR_ray_query` better than the RT pipeline. Ray queries need no SBT, can be issued from the existing compute lighting pass, and are what DDGI probe tracing, RT shadows and RT AO need. The RT pipeline can be deferred until path tracing.
- **First hardware-RT features:**
  - RT directional-light shadows (one ray/pixel with SIGMA-style denoise, about 0.4 ms denoise on high-end hardware per the NRD numbers);
  - RT AO;
  - DDGI probe updates.

  These need a BLAS/TLAS build pass in the render graph, including BLAS refit for skinned meshes after the skinning compute pass, which fits the existing skinning-before-mesh ordering.
- **Denoiser choice.** NRD is HLSL with a NRI-oriented API but has a documented Vulkan path. It can be compiled to SPIR-V and called as compute passes. Alternatively, write an SVGF-class denoiser in Slang, which is more work but avoids a vendor dependency.
- **ReSTIR DI fits the light system.** It would replace or augment clustered point-light shading for very many shadowed lights, and the existing froxel light lists could serve as ReSTIR candidate sources. ReSTIR GI and path tracing are "long-term / high-end PC only".

### Gaps
- No verified frame-time numbers for Cyberpunk Overdrive or Alan Wake 2 passes. NVIDIA and Remedy talks were not fetched.
- AMD FidelityFX Denoiser's current status and Vulkan support were not verified.
- DLSS Ray Reconstruction is proprietary and NVIDIA-only. Its integration requirements were not researched.

---

## 4. Screen-space techniques (GTAO / XeGTAO, visibility-bitmask SSIL, SSGI, SSR / stochastic SSR, contact shadows, Hi-Z)

### Takeaway
Screen-space AO and GI are the universal quick wins. They are pure compute on depth and normals and run on every target, including MoltenVK. XeGTAO (MIT, Intel) is the reference GTAO, at about 0.56 ms at 1080p on an RTX 2060. The 2023 visibility-bitmask extension (SSILVB) turns the same horizon walk into combined AO plus short-range indirect diffuse with thin-object handling. Stochastic SSR with Hi-Z tracing is the standard reflection layer above probes.

### Cited Findings
- **XeGTAO** **[verified from GitHub page fetched this session]**:
  - It is an MIT-licensed DX12/HLSL implementation of Jimenez et al. 2016 "Practical Realtime Strategies for Accurate Indirect Occlusion" (GTAO). It uses a depth MIP chain from McGuire et al. 2012 (SAO). Authors are Filip Strugar and Steve Mccalla.
  - Three compute passes: PrefilterDepths, MainPass and Denoise (a 5×5 depth-aware spatial filter, with temporal accumulation left to TAA).
  - Timings:
    - High preset: **~1.4 ms at 4K on RTX 3070**, **~0.56 ms at 1080p on RTX 2060**, **~2.39 ms at 1080p on i7-1195G7 iGPU**.
    - Medium is about 2/3 of High; Low is about 2/3 of Medium.
    - XeGTAO High is faster than ASSAO Medium (0.72 ms) on an RTX 2060.
    - Optional **bent normals** (v1.30) add about 25%. FP16 gives 5–20%.
  - High uses 3 slices × 6 samples.
  - **The repo was archived on 22 April 2024**. Intel discontinued it and takes no patches.
  - Source: [XeGTAO GitHub](https://github.com/GameTechDev/XeGTAO)
- **Screen Space Indirect Lighting with Visibility Bitmask (SSILVB).** Therrien, Levesque, Gilet, arXiv 2301.11376 and *The Visual Computer* (2023).
  - It replaces GTAO's two horizon angles with an N-sector occlusion bitmask plus a constant thickness. Light can pass behind thin surfaces, and AO, indirect diffuse and ambient improve "while minimizing noise and keeping a low performance overhead".
  - Sample code is in the author's blog post.
  - Sources: [arXiv](https://arxiv.org/abs/2301.11376), [slides](https://cdrinmatane.github.io/cgspotlight-slides/ssilvb_slides.pdf), [author site](https://cdrinmatane.github.io) **[verified via search summary]**
- **Background techniques** **[canonical, unverified; no source fetched]**:
  - **Stochastic SSR**: Stachowiak & Uludag, "Stochastic Screen-Space Reflections", SIGGRAPH 2015 Advances (Frostbite). It uses BRDF importance-sampled rays, Hi-Z ray marching, ratio-estimator spatial reuse and temporal filtering.
  - **AMD FidelityFX SSSR**: Stochastic Screen Space Reflections, an open-source Hi-Z SSR implementation on GPUOpen.
  - **Contact shadows**: short screen-space ray marches toward the light, as in UE4/5 and Days Gone's "Bend Studio screen space shadows".
  - **GTAO paper**: Jimenez, Wu, Pesce, Jarabo, "Practical Realtime Strategies for Accurate Indirect Occlusion", SIGGRAPH 2016 course / Activision tech report. It includes the multi-bounce AO approximation and specular occlusion from bent cones.

### Inferences
- **Highest-value item in this note.** GTAO, ideally in the visibility-bitmask form, is the single highest value-per-effort item for quat-3d. It is already on ROADMAP R8. It is about 0.5–1.5 ms on desktop and has no extension requirements, so it works on MoltenVK and KosmicKrisp. The HLSL ports cleanly to Slang compute, and TAA already exists for temporal accumulation.
- **Bent normals are worth the ~25%.** They feed specular occlusion for IBL and improve how the existing static and dynamic IBL probes look in crevices.
- **SSR needs Hi-Z first.** A Hi-Z depth pyramid is a shared prerequisite for SSR, SSGI, contact shadows and R6 occlusion culling. Build it once as a render-graph resource.
- **Screen-space limits.** Screen-space GI and SSR fail off-screen. They must fall back to world-space data (DDGI probes, IBL) rather than black, which is why the tiers below layer screen-space over world-space.

### Gaps
- No verified cost numbers for stochastic SSR, FidelityFX SSSR or SSILVB on reference GPUs.
- No primary source fetched for contact shadows, for example Bend Studio's GDC talk or open-source code.

---

## 5. Shadows (cascaded shadow maps, SDSM, stable cascades, virtual shadow maps, filtering: PCSS, EVSM, MSM)

### Takeaway
quat-3d currently has one directional shadow map, which is below the baseline. The industry baseline is:
- 4 cascades;
- either stable (texel-snapped, sphere-bounded) cascades or depth-bounds-driven SDSM partitions computed on GPU from the depth buffer;
- PCF/PCSS or moment-based filtering.

UE5's Virtual Shadow Maps (16k² virtual, page-cached clipmaps) are the high end. They depend on fast geometry submission (Nanite / GPU-driven rendering) and are long-term. RT shadows with denoising are the hardware-RT alternative.

### Cited Findings
- **SDSM.** Lauritzen, Salvi, Lefohn, "Sample Distribution Shadow Maps", I3D 2011 (first presented at SIGGRAPH 2010 Advances). It "optimizes the placement and size of a fixed number of Z-partitions using the distribution of the light space samples required by the current frame". — [Intel article](https://www.intel.com/content/www/us/en/developer/articles/technical/sample-distribution-shadow-maps.html), [SIGGRAPH 2010 notes](https://www.intel.com/content/dam/develop/external/us/en/documents/sampledistributionshadowmaps-siggraph2010-notes-181237.pdf) **[verified via search summary]**
- **Moment Shadow Mapping.** Peters & Klein, I3D 2015. It stores 4 depth moments for "high quality results with a single shadow map sample per fragment using 64 bits per shadow map texel". Like VSM and ESM, it is directly filterable (blur or mipmap). — [momentsingraphics.de](https://momentsingraphics.de/I3D2015.html) **[verified via search summary]**
- **Survey reference.** MJP's "A Sampling of Shadow Techniques" surveys and benchmarks CSM stabilisation, SDSM, PCF, EVSM and MSM, with an open-source demo. — [MJP blog](https://therealmjp.github.io/posts/shadow-maps) **[verified link; content from background knowledge]**
- **UE5 Virtual Shadow Maps** **[canonical, unverified; Epic docs host blocked]**:
  - Directional lights use a clipmap of 16384² virtual shadow maps split into 128×128 pages. Only pages needed by visible pixels are allocated and rendered.
  - Unchanged pages are cached across frames and invalidated by moving geometry.
  - Filtering is "Shadow Map Ray Tracing" (SMRT) for soft shadows.
  - Designed for Nanite. Non-Nanite geometry works at much higher cost.
  - Source: [Epic VSM docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)
- **PCSS.** Fernando, "Percentage-Closer Soft Shadows", NVIDIA 2005. It is the standard blocker-search plus variable-kernel PCF. — **[canonical, unverified; no source fetched]**

### Inferences
- **Medium priority, required before most GI work.** Upgrade to 4-cascade CSM as the CLAUDE.md frame structure already specifies.
  - Compute the depth bounds (SDSM-lite) in a compute reduction over the depth buffer each frame. This keeps it GPU-driven with no CPU readback, by writing cascade matrices to a buffer.
  - Snap cascades to texels to stop shimmer under TAA.
  - Use PCF with receiver-plane depth bias, plus optional PCSS for the sun.
  - DDGI and all GI techniques need correct direct-light shadowing to inject bounce light, so this comes before them.
- **Filtering choice.** MSM or EVSM is attractive if quat-3d wants pre-filtered shadows for vegetation and terrain, at the cost of light-leak tuning. PCF/PCSS is simpler and the more robust default.
- **Virtual shadow maps are long-term.** They pair naturally with R6/R9.4 GPU-driven culling and instancing, because each page needs its own culled draw list. Revisit after GPU-driven rendering lands.
- **Contact shadows.** A screen-space ray march (§4) fixes small-scale detail that CSM resolution misses, especially on vegetation and characters, and is cheap.

### Gaps
- No verified VSM cost numbers or page-size details. The Epic docs could not be reached.
- No primary-source numbers on SDSM versus fixed-split quality or cost on modern GPUs.

---

## 6. Hardware / API requirements and macOS (MoltenVK, KosmicKrisp) constraints

### Takeaway
As of late 2026, there is no evidence that either Vulkan-on-Metal path exposes `VK_KHR_acceleration_structure` / `VK_KHR_ray_query`:
- **MoltenVK**: its changelog up to v1.4.2 (released 2026-07-20) and the in-progress v1.4.3 has no ray-tracing entries.
- **KosmicKrisp**: Vulkan 1.4 conformant in October 2026, but lists ray tracing only as a possible future goal.

Any GI tier that macOS must run therefore has to be compute-only (screen-space, probes with raster or SDF tracing). Hardware RT is a Windows/Linux-only tier until the native Metal backend (ROADMAP P2/P5) can use Metal's own ray-tracing API.

### Cited Findings
- **MoltenVK changelog.** `Docs/Whats_New.md` (fetched this session) covers v1.2.8 (2024-03-12) through v1.4.1 (2025-11-24), v1.4.2 (**released 2026-07-20**) and v1.4.3 (TBD). A grep for "ray" or "acceleration" matched only the word "array", so no ray-tracing feature entries exist. — [MoltenVK Whats_New.md](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md) **[verified]**
- **Conflicting MoltenVK dates.** Wikipedia lists MoltenVK 1.4.2 as 30 November 2025, but the repo changelog says 2026-07-20. The changelog is authoritative. — [Wikipedia](https://en.wikipedia.org/wiki/MoltenVK) contradicted by [MoltenVK Whats_New.md](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md)
- **MoltenVK RT history.** At Vulkanised 2024 the maintainer listed "Vulkan Acceleration Structures (initial ray tracing)" as a work item and "Exploring ray-tracing funding". The MoltenVK GitHub RT issue states that ray tracing is not supported. Metal 3's buffer device address (a prerequisite) was added in 2023. — [Vulkanised 2024 slides](https://dev.vulkan.org/user/pages/09.events/vulkanised-2024/vulkansied-2024-bill-hollings-brenwill.pdf), [MoltenVK issue #427](https://github.com/KhronosGroup/MoltenVK/issues/427) **[verified via search summary]**
- **SPIRV-Cross RT translation.** SPIRV-Cross can translate `SPV_KHR_ray_query` to Metal 2.4 intersection queries. The translation exists, but MoltenVK does not expose the extension. — [SPIRV-Cross commit](https://fuchsia.googlesource.com/third_party/spirv-cross/+/597f29d09d3d49fca9f3654c43210ca0cb3b5371%5E%21/) **[verified via search summary]**
- **KosmicKrisp** **[verified via search summary]**:
  - LunarG's Mesa Vulkan-on-Metal driver passed Vulkan 1.4 CTS (Phoronix, 3 Oct 2026). It requires Apple Silicon (M1+) and macOS 26 (Metal 4).
  - LunarG's XDC 2025 overview lists future goals: "Vulkan 1.4 support, sparse memory, tessellation, mesh shaders, and potentially ray tracing". The XDC 2025 deck literally asks "Ray tracing???".
  - Sources: [Khronos news](https://www.khronos.org/news/permalink/kosmickrisp-achieves-vulkan-1.4-conformance-on-apple-silicon), [Phoronix](https://www.phoronix.com/news/KosmicKrisp-Vulkan-1.4-2026), [LunarG XDC 2025](https://www.lunarg.com/lunarg-at-xdc-2025-kosmickrisp-overview/), [XDC 2025 slides](https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf)
- **Arm mobile GPUs.** Mobile GPUs expose ray query. Arm's learning path covers `VK_KHR_ray_query` traversal on Mali, so ray-query-only designs also reach Android high-end. — [Arm Learning Paths](https://learn.arm.com/learning-paths/mobile-graphics-and-gaming/ray_tracing/rt03_ray_traversal/) **[verified via search summary]**

### Inferences
- **Requirements by technique:**

  | Technique | Vulkan requirement | macOS (MoltenVK/KK) |
  |---|---|---|
  | GTAO/XeGTAO, SSILVB, SSR/SSSR, contact shadows, Hi-Z | Core 1.3 compute | Yes |
  | CSM/SDSM/PCF/PCSS/MSM | Core 1.3 (depth formats; MSM needs RGBA16/32F filterable) | Yes |
  | Baked SH/irradiance volume, raster-updated probes | Core | Yes |
  | DDGI with SDF/voxel tracer, Brixelizer GI, SDFGI/HDDAGI-style | Core compute + 3D textures (+ atomics) | Very likely yes (unverified for 64-bit atomics) |
  | DDGI with HW tracer, RT shadows/AO/reflections | `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (+ `VK_KHR_deferred_host_operations`) | No (as of Nov 2026) |
  | ReSTIR DI/GI, path tracing, SHaRC/NRC | Above + often `VK_KHR_ray_tracing_pipeline`; NRC needs tensor-core paths | No |
  | Virtual shadow maps | Core + GPU-driven indirect draw; sparse/atomics helpful | Probably (sparse resources not in KK yet) |

- **Design the GI interface around tracers.** The GI system's world-space cache and screen gather should be backend-agnostic, with a pluggable "trace rays against scene" stage that has an SDF/compute implementation and a ray-query implementation. That keeps macOS on the same GI data path. It is also what Lumen (SW/HW RT) and AMD (GI-1.0 HW vs Brixelizer GI SDF) do.

### Gaps
- KosmicKrisp's exact advertised extension list could not be read because the Mesa docs host was blocked.
- Whether the native Metal backend would gain Metal RT for parity is a project decision, not researched.

---

## 7. Recommended overall GI strategy and tiering for quat-3d

### Takeaway
Use a three-tier, layered lighting stack that shares one world-space radiance-cache interface:
- **Tier 0 (all GPUs incl. macOS/iGPU):** CSM + GTAO/SSILVB + IBL + a static or slowly relit probe volume.
- **Tier 1 (all desktop + macOS, compute-only dynamic GI):** DDGI-style probe volume traced against a global sparse SDF (Brixelizer-style) plus SSR. This is the main shipping target.
- **Tier 2 (RT GPUs on Windows/Linux):** swap the tracer to `VK_KHR_ray_query`, then add RT shadows/AO/reflections with NRD-class denoising.
- **Tier 3 (long-term):** ReSTIR DI/GI and path tracing.

Build Tier 0 quick wins first, because they are prerequisites, not throwaway work.

### Cited Findings
- **Shipping engines split trace and cache.** Lumen (software SDF and hardware RT paths) and AMD's GI-1.0 (HW RT) vs Brixelizer GI (compute SDF) both keep one cache/gather architecture with two tracers. — [Epic Lumen in Fortnite](https://www.unrealengine.com/en-US/tech-blog/lumen-brings-real-time-global-illumination-to-fortnite-battle-royale-chapter-4), [GDC 2024 Brixelizer GI](https://gpuopen.com/download/GDC2024_GI_with_AMD_FidelityFX_Brixelizer.pdf)
- **Budget reference.** Production 60 fps budgets for all dynamic GI plus reflections are about **4 ms** (Fortnite / Lumen). — [Epic](https://www.unrealengine.com/en-US/tech-blog/lumen-brings-real-time-global-illumination-to-fortnite-battle-royale-chapter-4)
- **Reference costs:**
  - GI-1.0: about 1.9–3.1 ms on RX 6900 XT — [arXiv](https://ar5iv.labs.arxiv.org/html/2310.19855)
  - XeGTAO: about 0.56 ms at 1080p on RTX 2060 — [XeGTAO](https://github.com/GameTechDev/XeGTAO)
  - NRD SIGMA shadow denoise: about 0.4 ms at 1440p on RTX 4080 — [NRD](https://github.com/NVIDIA-RTX/NRD)

### Inferences

**Prioritised catalogue for quat-3d.** All of these are inferences built from the findings above.

| # | Technique | Priority | Est. cost / complexity | Prereqs | Notes |
|---|---|---|---|---|---|
| 1 | **Hi-Z depth pyramid** (compute) | Quick win | Low | — | Shared by SSR, SSGI, contact shadows and R6 occlusion culling |
| 2 | **GTAO (XeGTAO port) + bent normals, upgrade to visibility bitmask (SSILVB)** | Quick win (R8) | About 0.5–1.5 ms; low–medium | Hi-Z/depth MIPs, normals, TAA | Specular occlusion for IBL; SSILVB adds short-range indirect diffuse |
| 3 | **4-cascade CSM, GPU depth-bounds (SDSM-lite), texel snapping, PCF/PCSS** | Quick win → medium | Medium | Compute reduction | Required for credible GI injection; already in the CLAUDE.md frame plan |
| 4 | **Contact shadows** (screen-space march) | Quick win (R8) | Low | Depth | Vegetation and character detail |
| 5 | **Stochastic SSR (Hi-Z, FidelityFX SSSR-style)** with IBL / probe fallback | Medium (R8) | Medium; about 1–2 ms (unverified) | Hi-Z, TAA | Falls back to dynamic IBL probe |
| 6 | **Probe volume (L1/L2 SH or octahedral irradiance + depth), camera-relative cascades, probe relocation/classification**, initially updated by raster or sky-only relighting | Medium | Medium | CSM | The GI data structure that every later tier reuses |
| 7 | **Global sparse SDF (Brixelizer-style bricks) + DDGI probe tracing + SDF reflections**; heightfield terrain traced analytically | Long-term (main dynamic GI) | High | 6, compute SDF build per frame | Runs on MoltenVK/KK; reference: Brixelizer GI (MIT, Vulkan) |
| 8 | **Screen-probe final gather + world radiance cache (GI-1.0 / Lumen-style)** | Long-term | High | 7 | Raises indirect detail beyond probe-grid resolution |
| 9 | **Ray-query tracer backend** (BLAS/TLAS in render graph, skinned BLAS refit) feeding 7/8; RT sun shadows + RT AO with SIGMA/ReBLUR-class denoise | Medium-long (RT tier) | Medium-high | `VK_KHR_ray_query`; denoiser | Windows/Linux only; the macOS gap is a Metal backend question |
| 10 | **ReSTIR DI (RTXDI-style)** for many shadowed lights, using froxel light lists as candidates | Long-term | High | 9 | Complements clustered shading |
| 11 | **ReSTIR GI / SHaRC / path tracing + ReLAX/ReBLUR** | Long-term / experimental | Very high | 9, 10 | High-end PC showcase mode |
| 12 | **Virtual shadow maps** | Long-term | High | R6/R9.4 GPU-driven culling | Replaces CSM once GPU-driven rendering exists |

**Key strategy points:**
- **One GI interface, pluggable tracers.** Tiers differ only in how probe and screen-probe rays are traced: SDF/compute versus ray query. The engine keeps one radiance-cache data model, one denoise/temporal path and one lighting-pass integration. This avoids per-platform GI forks and matches CLAUDE.md's wish to keep Vulkan types out of renderer-facing APIs for the future Metal backend.
- **Order matters.** Shadows (CSM) and AO come before GI, because GI multiplies whatever direct lighting and occlusion errors exist. Hi-Z comes before SSR and occlusion culling.
- **Keep a CPU or offline reference.** Per CLAUDE.md's "keep a CPU reference where determinism matters", provide a ground-truth path-traced or brute-force probe bake (offline compute) for regression tests of probe GI and GTAO. These would be headless image-diff tests on lavapipe, which does not support RT, so the compute tiers are the testable ones in CI.
- **Avoid VCT/SVOGI.** Voxel cone tracing is superseded in practice by SDF + probe caches: SDFGI → HDDAGI in Godot, Lumen and Brixelizer GI.

### Gaps
- Per-technique ms on Apple Silicon or MoltenVK were not found for any technique. A quat-3d benchmark would be needed.
- Brixelizer GI, stochastic SSR and VSM ms costs on reference GPUs are unverified. Primary decks exist but could not be read in this session.
- Several canonical sources could not be fetched because their hosts were blocked in this session: jcgt.org (DDGI), advances.realtimerendering.com (Lumen 2022, SSR 2015, CoD talks), dev.epicgames.com (Lumen/VSM docs) and docs.mesa3d.org (KosmicKrisp extensions). Allowing those hosts and re-checking would raise confidence in the items marked "[canonical, unverified]".
