# Reading list

The papers, talks and articles behind the techniques in quat-3d, grouped by
area. Each entry says where the technique lives in the engine, so you can read
the source next to the paper.

**Status:** ✅ in the engine · 🔧 in an open PR · 🗺 planned on the [ROADMAP](../../ROADMAP.md)

For a broader survey of state-of-the-art techniques we have not adopted yet,
see the [real-time rendering tech radar](../research/reports/Real%20time%20rendering%20tech%20radar.md).

**Keeping it current:** when you implement a published technique, cite it in a
comment at the implementation and add it here in the same PR.

---

## Where to start

If you read only a few things, read these. They underpin most of the engine.

1. **Real-Time Rendering, 4th ed.** — Akenine-Möller, Haines, Hoffman et al.,
   2018. The reference text; chapters 9 (PBR), 11 (global illumination),
   12 (post) and 14 (volumetrics) map directly onto our renderer.
   [realtimerendering.com](https://www.realtimerendering.com/)
2. **Real Shading in Unreal Engine 4** — Karis, SIGGRAPH 2013 shading course.
   The BRDF and image-based-lighting model most engines (including ours) use.
   [notes (PDF)](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf)
3. **Moving Frostbite to Physically Based Rendering 3.0** — Lagarde & de Rousiers,
   SIGGRAPH 2014. The most complete practical PBR write-up: units, lights,
   materials, IBL. [course notes (PDF)](https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf)
   Its camera chapter (EV100 from aperture, shutter and ISO) is R20's
   physical exposure: `src/renderer/physical_camera.cpp`.
4. **Physically Based Rendering: From Theory to Implementation, 4th ed.** —
   Pharr, Jakob, Humphreys. Free online; the theory behind microfacets,
   sampling and volumes. [pbr-book.org](https://pbr-book.org/4ed/contents)
5. **Advances in Real-Time Rendering** course archive (SIGGRAPH, 2006–today).
   Where studios publish what they ship. [advances.realtimerendering.com](https://advances.realtimerendering.com/)
   — and Stephen Hill's archive of the PBR shading courses:
   [blog.selfshadow.com/publications](https://blog.selfshadow.com/publications/)

---

## Engine architecture

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **FrameGraph: Extensible Rendering Architecture in Frostbite** — O'Donnell, GDC 2017. [GDC Vault](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in) | Render graph: `src/rhi/render_graph.cpp`, `include/engine/rhi/render_graph.hpp` |
| ✅ | **ECS back and forth** (series) — Caini (author of entt), 2019. [part 1](https://skypjack.github.io/2019-02-14-ecs-baf-part-1/) | ECS on entt: `src/scene/` |
| 🗺 | **GPU-Driven Rendering Pipelines** — Haar & Aaltonen, SIGGRAPH 2015. [PDF](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf) | R5 GPU-driven rendering, R6 GPU culling |
| 🗺 | **Nanite: A Deep Dive** — Karis, Stubbe, Wihlidal, SIGGRAPH 2021. [PDF](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) | R19 virtual geometry |

## Vulkan & GPU architecture

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **A trip through the Graphics Pipeline 2011** — Giesen. A 13-part tour of what the GPU and driver actually do with your draw calls. [index](https://fgiesen.wordpress.com/2011/07/09/a-trip-through-the-graphics-pipeline-2011-index/) | Background for all of `src/rhi/` and `src/renderer/` |
| ✅ | **Life of a triangle — NVIDIA's logical pipeline** — Christoph Kubisch (NVIDIA), 2015. [article](https://developer.nvidia.com/content/life-triangle-nvidias-logical-pipeline) | Background (warps, SMs, raster) |
| ✅ | **From Shader Code to a Teraflop: How GPU Shader Cores Work** — Fatahalian, SIGGRAPH *Beyond Programmable Shading* course (2009 and 2010 editions). SIMD lanes, latency hiding, why divergence costs. [SIGGRAPH 2010 slides (PDF)](http://bps10.idav.ucdavis.edu/talks/03-fatahalian_gpuArchTeraflop_BPS_SIGGRAPH2010.pdf) | Every compute pass |
| ✅ | **Occupancy explained** — Guthmann (AMD GPUOpen). Registers, LDS and waves in flight. [gpuopen.com](https://gpuopen.com/learn/occupancy-explained/) | Compute passes (particles, grass, terrain, GTAO) |
| ✅ | **Vulkan Guide** (Khronos) and **Synchronization Examples**. [docs.vulkan.org/guide](https://docs.vulkan.org/guide/latest/index.html) · [sync examples](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples) | `src/rhi/` (sync2, dynamic rendering, descriptor buffer) |
| ✅ | **Yet another blog explaining Vulkan synchronization** — Themaister (Arntzen), 2019. [blog](https://themaister.net/blog/2019/08/14/yet-another-blog-explaining-vulkan-synchronization/) | Render-graph barrier insertion, `src/rhi/render_graph.cpp` |
| ✅ | **Breaking Down Barriers** (series) — Pettineo (MJP), 2018. What a GPU barrier really does in hardware. [part 1](https://therealmjp.github.io/posts/breaking-down-barriers-part-1-whats-a-barrier/) | same |
| ✅ | **Writing an efficient Vulkan renderer** — Kapoulkine, 2020 (blog edition of his *GPU Zen 2* chapter, 2019). Memory, descriptors, pipelines, command submission, sync. [zeux.io](https://zeux.io/2020/02/27/writing-an-efficient-vulkan-renderer/) | `src/rhi/` design |
| ✅ | **Vulkan Memory Allocator documentation** — AMD GPUOpen. [docs](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/) | `src/rhi/gpu_allocator.cpp`, `vma_impl.cpp` |
| ✅ | **VK_EXT_descriptor_buffer** — Khronos Vulkan blog, November 2022. [vulkan.org](https://vulkan.org/blog/vk_ext_descriptor_buffer) · [extension proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html) | `src/rhi/descriptor_buffer.cpp` |

## Low-level CPU performance & concurrency

| | Reading | In quat-3d |
|---|---|---|
| 🗺 | **What Every Programmer Should Know About Memory** — Drepper, 2007. [PDF](https://www.akkadia.org/drepper/cpumemory.pdf) | R4.5 data-oriented CPU pass, added to the ROADMAP by #82 (caches, alignment, false sharing) |
| 🗺 | **Data-Oriented Design and C++** — Acton, CppCon 2014. [video](https://www.youtube.com/watch?v=rX0ItVEVjHc) | R4.5 data-oriented CPU pass |
| 🗺 | **Data-Oriented Design** (book) — Fabian. Free online. [dataorienteddesign.com](https://www.dataorienteddesign.com/dodbook/) | R4.5 |
| 🗺 | **Gallery of Processor Cache Effects** — Ostrovsky, 2010. Seven tiny experiments that make cache behaviour visible. [blog](https://igoro.com/archive/gallery-of-processor-cache-effects/) | R4.5 |
| 🗺 | **Software optimization resources** — Fog. Instruction tables, microarchitecture guide, C++ optimisation manual. [agner.org/optimize](https://www.agner.org/optimize/) | R4.5 (SIMD, branch costs) |
| ✅ | **An Introduction to Lock-Free Programming** and the memory-ordering series — Preshing, 2012. [preshing.com](https://preshing.com/20120612/an-introduction-to-lock-free-programming/) | Acquire/release atomics: asset load state (`include/engine/asset/asset_manager.hpp`), shader hot-reload worker hand-off (`src/rhi/shader_hot_reload.cpp`) |
| 🗺 | **Parallelizing the Naughty Dog Engine Using Fibers** — Gyrling (Naughty Dog), GDC 2015. [GDC Vault](https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine) | An engine job system (not on the ROADMAP yet; today Jolt runs its own `JobSystemThreadPool` in `src/physics/physics_world.cpp`, and R1 plans thread-pool asset loads) |
| 🗺 | **Job System 2.0: Lock-Free Work Stealing** (series) — Reinalter (Molecular Matters), 2015. [part 1](https://blog.molecular-matters.com/2015/08/24/job-system-2-0-lock-free-work-stealing-part-1-basics/) | Engine job system (as above) |

## Instrumentation, profiling & GPU debugging

| | Reading | In quat-3d |
|---|---|---|
| 🔧 | **Tracy profiler manual** — Taudul. [manual (PDF)](https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf) · [github.com/wolfpld/tracy](https://github.com/wolfpld/tracy) | R4 profiling: CPU/GPU zones, `profile.*` remote + MCP tools (#82) |
| 🔧 | **Timestamp queries** — Vulkan specification, *Queries → Timestamp Queries*. [docs.vulkan.org](https://docs.vulkan.org/spec/latest/chapters/queries.html#queries-timestamps) | R4 per-pass GPU timing via `vkCmdWriteTimestamp2` (#82) |
| ✅ | **RenderDoc documentation** — Karlsson. Frame capture and GPU debugging. [renderdoc.org/docs](https://renderdoc.org/docs/) | Debugging any pass (external tool; no in-engine capture hooks) |
| ✅ | **Flame Graphs** — Gregg. Reading CPU profiles (and his *Systems Performance* book for the full method). [brendangregg.com](https://www.brendangregg.com/flamegraphs.html) | Reading CPU sampling profiles of the engine (e.g. Linux `perf`; not wired into the build) |
| ✅ | **Validation layers guide** — Khronos Vulkan Guide. [docs.vulkan.org](https://docs.vulkan.org/guide/latest/validation_overview.html) | Debug builds enable the validation layer (`src/app/application.cpp`); `engine_tests --validation` or `QUAT_VALIDATION=1` runs the GPU tests under it |

## Materials & shading (PBR)

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Microfacet Models for Refraction through Rough Surfaces** — Walter et al., EGSR 2007 (the GGX distribution). [PDF](https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.pdf) | `assets/shaders/lighting.slang`, `lib/pbr.slang` |
| ✅ | **Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs** — Heitz, JCGT 2014 (height-correlated Smith). [JCGT](https://jcgt.org/published/0003/02/03/) | `lib/pbr.slang` |
| ✅ | **Physically-Based Shading at Disney** — Burley, SIGGRAPH 2012. [PDF](https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf) | Material parameterisation |
| ✅ | **Real Shading in Unreal Engine 4** — Karis 2013 (see *Where to start*) | Split-sum IBL, `lib/pbr.slang` |
| ✅ | **Filtering Distributions of Normals for Shading Antialiasing** — Kaplanyan, Hill, Patney, Lefohn, HPG 2016; refined by Tokuyoshi & Kaplanyan, I3D 2019. [NVIDIA Research](https://research.nvidia.com/publication/2016-06_filtering-distributions-normals-shading-antialiasing) | Geometric specular AA, `lib/pbr.slang` |
| ✅ | **Revisiting Physically Based Shading at Imageworks** — Kulla & Conty, SIGGRAPH 2017 (energy-conserving multiple scattering). [course page](https://blog.selfshadow.com/publications/s2017-shading-course/) | Punctual-light compensation (Filament's form): `assets/shaders/lib/brdf_energy.slang`, `lighting.slang` |
| ✅ | **A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting** — Fdez-Agüera, JCGT 2019. [JCGT](https://jcgt.org/published/0008/01/03/) | `lib/brdf_energy.slang` (`ibl_energy`), `lighting.slang`; the DFG LUT it reads, `ibl_brdf_lut.slang` |
| ✅ | **Horizon Occlusion for Normal Mapped Reflections** — Russell (Marmoset), 2014, as used in Jimenez et al., *Practical Realtime Strategies for Accurate Indirect Occlusion*, SIGGRAPH 2016. [Marmoset](https://marmosetco.tumblr.com/post/81245981087) | `lib/brdf_energy.slang` (`horizon_occlusion`), geometric normal from depth in `lighting.slang` |
| ✅ | **Real-time Shading with Filtered Importance Sampling** — Křivánek & Colbert, GPU Gems 3 ch. 20, 2008. [NVIDIA](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-20-gpu-based-importance-sampling) | HDRI GGX prefilter: `assets/shaders/env_prefilter.slang` (R21) |
| ✅ | **An Efficient Representation for Irradiance Environment Maps** — Ramamoorthi & Hanrahan, SIGGRAPH 2001. [PDF](https://cseweb.ucsd.edu/~ravir/papers/envmap/envmap.pdf) | SH9 irradiance: `assets/shaders/lib/sh9.slang`, `sh_project.slang`, `sh_irradiance.slang` (R21) |

## Lighting pipeline

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Clustered Deferred and Forward Shading** — Olsson, Billeter, Assarsson, HPG 2012. [preprint (PDF)](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf) | `assets/shaders/light_cull.slang`, `lib/clustered_lights.slang`, `include/engine/renderer/light_clusters.hpp` |
| ✅ | **A Primer On Efficient Rendering Algorithms & Clustered Shading** — Ortiz, 2018. Approachable walkthrough. [blog](https://www.aortiz.me/2018/12/21/CG.html) | same |
| 🗺 | **Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields** (DDGI) — Majercik et al., JCGT 2019. [JCGT](https://jcgt.org/published/0008/02/01/) | GI track (R-track, see tech radar) |
| 🗺 | **ReSTIR GI: Path Resampling for Real-Time Path Tracing** — Ouyang et al., HPG 2021. [NVIDIA Research](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing) | GI track |
| 🗺 | **Lumen: Real-time Global Illumination in Unreal Engine 5** — Wright et al., SIGGRAPH 2022. [PDF](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf) | GI track |

## Shadows & ambient occlusion

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Parallel-Split Shadow Maps on Programmable GPUs** — Zhang, Sun, Nyman, GPU Gems 3 ch. 10 (2007). [online](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus) | Cascade splits, `include/engine/renderer/csm.hpp` |
| ✅ | **Sample Distribution Shadow Maps** — Lauritzen, Salvi, Lefohn, I3D 2011 / SIGGRAPH 2010 course. [slides (PDF)](https://www.advances.realtimerendering.com/s2010/Lauritzen-SDSM(SIGGRAPH%202010%20Advanced%20RealTime%20Rendering%20Course).pdf) | Depth-bounds-fitted cascades, `csm.hpp` |
| ✅ | **Stable Cascaded Shadow Maps** — Valient, ShaderX6 (2008); also Microsoft's **Cascaded Shadow Maps** article. [learn.microsoft.com](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps) | Texel snapping, `csm.hpp` |
| ✅ | **Practical Realtime Strategies for Accurate Indirect Occlusion** (GTAO) — Jimenez, Wu, Pesce, Jarabo, SIGGRAPH 2016. [PDF](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf) | `assets/shaders/gtao.slang`, `lib/occlusion.slang` (multi-bounce, eq. 10) |
| ✅ | **XeGTAO** — Strugar & McCalla (Intel), MIT-licensed reference implementation we port. [GitHub](https://github.com/GameTechDev/XeGTAO) | `gtao.slang`, `gtao_denoise.slang` |
| ✅ | **Accurate Normal Reconstruction from Depth Buffer** — Yuwen Wu, 2021. [blog](https://atyuwen.github.io/posts/normal-reconstruction) | `lib/occlusion.slang` contact shadows: `depth_slope` picks each axis's neighbour on the texel's own surface by second difference (#86) |
| 🔧 | **Hashed Alpha Testing** — Wyman & McGuire, I3D 2017. [NVIDIA Research](https://research.nvidia.com/labs/rtr/publication/wyman2017hashed) | Alpha MASK cut-outs (#70); planned: R25.2 seasonal leaf drop |
| 🔧 | **Anti-aliased Alpha Test: The Esoteric Alpha To Coverage** — Golus, 2017 (coverage-preserving mips). [Medium](https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f) | #70 |

## Sky, atmosphere, clouds & fog

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **A Scalable and Production Ready Sky and Atmosphere Rendering Technique** — Hillaire, EGSR 2020. [publications page](https://sebh.github.io/publications/) · [reference code](https://github.com/sebh/UnrealEngineSkyAtmosphere) | `lib/atmosphere.slang`, `atmos_multiscatter.slang`, `atmos_skyview.slang` |
| ✅ | **Precomputed Atmospheric Scattering** — Bruneton & Neyret, EGSR 2008. [HAL](https://hal.archives-ouvertes.fr/inria-00288758) | Transmittance LUT parameterisation, `lib/atmosphere.slang` |
| 🗺 | **The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn** — Schneider & Vos, SIGGRAPH 2015. [Guerrilla](https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn) | Volumetric clouds v2 (queued) |
| 🗺 | **Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine** — Schneider, SIGGRAPH 2017. [Guerrilla](https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine) | same |
| 🗺 | **Nubis, Evolved** (2022) and **Nubis³: Methods (and madness) to model and render immersive real-time voxel-based clouds** (2023) — Schneider. [Nubis Evolved](https://www.guerrilla-games.com/read/nubis-evolved) · [Nubis³](https://www.guerrilla-games.com/read/nubis-cubed) | same |
| 🗺 | **Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite** — Hillaire, SIGGRAPH 2016. [PDF](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf) | Cloud lighting (multi-scattering, powder) |
| 🗺 | **Volumetric Fog: Unified Compute Shader-Based Solution to Atmospheric Scattering** — Wronski, SIGGRAPH 2014. [PDF](https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf) | R13 froxel fog |
| 🗺 | **Physically Based and Unified Volumetric Rendering in Frostbite** — Hillaire, SIGGRAPH 2015. [EA](https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite) | R13 froxel fog |

## Astronomy & time of day

The world clock and ephemerides of R22.1 (`src/astro/`, `src/scene/world_clock.cpp`) and the night sky planned for R22.2.

| | Reading | In quat-3d |
|---|---|---|
| 🔧 | **Astronomical Algorithms, 2nd ed.** — Meeus, Willmann-Bell, 1998 (ISBN 978-0-943396-61-3). Julian day (ch. 7), sidereal time (12), coordinate transforms (13), refraction (16), precession (21), nutation (22), the low-accuracy Sun and the equation of time (25, 28), the Moon's position (47), phase and bright limb (48), libration (53); its worked examples are our test cases | `src/astro/`, `tests/astro/` (#87) |
| 🔧 | **The lunar ephemeris ELP 2000** — Chapront-Touzé & Chapront, A&A 124, 50, 1983 (ELP-2000/82, the theory behind Meeus' tables 47.A/47.B). [ADS](https://ui.adsabs.harvard.edu/abs/1983A%26A...124...50C/abstract) | `src/astro/moon_terms.cpp`, `src/astro/moon.cpp` (#87) |
| 🔧 | **Astronomical refraction** — Sæmundsson, Sky & Telescope, July 1986, p. 70. The refraction-from-true-altitude formula (Meeus 16.4) | `refraction()`, `src/astro/coords.cpp` (#87) |
| 🔧 | **Text-Book on Spherical Astronomy** — W. M. Smart, Cambridge University Press (the 1956 edition Meeus cites for 28.3). The equation of time from L0, M, e and ε | `equation_of_time_min`, `src/astro/sun.cpp` (#87) |
| 🔧 | **NOAA Solar Calculator** — NOAA Global Monitoring Laboratory; the same low-accuracy solar algorithm (after Meeus). [calculator](https://gml.noaa.gov/grad/solcalc/) · [calculation details](https://gml.noaa.gov/grad/solcalc/calcdetails.html) | `src/astro/sun.cpp` (#87) |
| 🔧 | **Astronomy on the Personal Computer, 4th ed.** — Montenbruck & Pfleger, Springer, 2000. Rise / set / twilight by scanning the altitude, interpolating each step and refining crossings and grazing extrema | `src/astro/events.cpp` (#87) |
| 🔧 | **Five Millennium Canon of Solar Eclipses** (ΔT polynomials) — Espenak & Meeus, NASA/TP-2006-214141. [ΔT expressions](https://eclipse.gsfc.nasa.gov/SEcat5/deltatpoly.html) | `delta_t_seconds`, `src/astro/time.cpp` (#87) |
| 🔧 | **chrono-Compatible Low-Level Date Algorithms** — Hinnant. [article](https://howardhinnant.github.io/date_algorithms.html) | `days_from_civil` / `civil_from_days` (#87) |
| 🔧 | **A model of the brightness of moonlight** — Krisciunas & Schaefer, PASP 103, 1033, 1991. [DOI](https://doi.org/10.1086/132921) | Moon illuminance, `src/astro/moon.cpp` (#87) |
| 🔧 | **Revised optical air mass tables and approximation formula** — Kasten & Young, Applied Optics 28(22), 4735, 1989. [DOI](https://doi.org/10.1364/AO.28.004735) | `airmass`, `src/astro/coords.cpp` (#87) |
| 🔧 | **Reference luminous solar constant and solar luminance for illuminance calculations** — Darula, Kittler & Gueymard, Solar Energy 79(5), 559, 2005. [DOI](https://doi.org/10.1016/j.solener.2005.01.004) | Sun illuminance (133 334 lx), `src/astro/sun.cpp` (#87) |
| 🗺 | **A Physically-Based Night Sky Model** — Jensen, Durand, Stark, Premože, Dorsey & Shirley, SIGGRAPH 2001. The canonical reference for R22.2: stars, moonlight, airglow, zodiacal light, scotopic viewing | R22.2 night sky |
| 🗺 | **Model for artificial night-sky illumination** — Garstang, PASP 98, 1986 | R22.2 light-pollution skyglow |
| 🗺 | **Theory of Reflectance and Emittance Spectroscopy** — Hapke, Cambridge University Press (2nd ed. 2012). Lunar BRDF and the opposition surge | R22.2 Moon shading |
| 🗺 | **A Spatial Post-Processing Algorithm for Images of Night Scenes** — Thompson, Shirley & Ferwerda, Journal of Graphics Tools 7(1), 2002 | R22.2 night perception (blue shift, acuity loss, noise) |
| 🗺 | **Perceptually Based Tone Mapping for Low-Light Conditions** — Kirk & O'Brien, SIGGRAPH 2011 | R22.2 scotopic / mesopic tone mapping |

## Anti-aliasing, post-processing & camera

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **High Quality Temporal Supersampling** — Karis, SIGGRAPH 2014. [course page](https://advances.realtimerendering.com/s2014/) | `assets/shaders/taa.slang` |
| ✅ | **An Excursion in Temporal Supersampling** — Salvi, GDC 2016 (variance clipping). [PDF](https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf) | `taa.slang` |
| ✅ | **Temporal AA and the Quest for the Holy Trail** — López, 2022. Approachable TAA overview. [blog](https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/) | `taa.slang`; TAAU upscaler (R11) |
| ✅ | **Next Generation Post Processing in Call of Duty: Advanced Warfare** — Jimenez, SIGGRAPH 2014 (bloom, motion blur, DoF, interleaved gradient noise). [iryoku.com](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/) | `bloom_down.slang`; R20: `dof_*.slang` (CoC tiles, near/far gather, composite), `motion_blur*.slang` sampling and IGN jitter (#79) |
| ✅ | **ACES Filmic Tone Mapping Curve** — Narkowicz, 2016. [blog](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/) | `assets/shaders/lib/color.slang` (`AcesTonemapper`), CPU reference `src/renderer/color_grading.cpp` |
| 🔧 | **AgX** — Sobotka's view transform as Blender 4 ships it, and **Minimal AgX implementation** (iolite). [AgX](https://github.com/sobotka/AgX) · [minimal implementation](https://iolite-engine.com/blog_posts/minimal_agx_implementation) | R20 `AgxTonemapper` in `lib/color.slang`, CPU reference in `color_grading.cpp` (#79) |
| 🔧 | **A Reconstruction Filter for Plausible Motion Blur** — McGuire, Hennessy, Bukowski & Osman, I3D 2012 (TileMax / NeighborMax, cone and cylinder weights, soft depth comparison). [PDF](https://casual-effects.com/research/McGuire2012Blur/McGuire12Blur.pdf) | R20 `motion_blur_tiles.slang`, `motion_blur_neighbor.slang`, `motion_blur.slang` (#79) |
| 🔧 | **A Fast and Stable Feature-Aware Motion Blur Filter** — Guertin, McGuire & Nowrouzezahrai, HPG 2014 (alternate sampling along the pixel's own vector). [project page](https://casual-effects.com/research/Guertin2013MotionBlur/) | R20 `motion_blur.slang` (#79) |
| 🔧 | **Khronos PBR Neutral Tone Mapper** — Khronos 3D Commerce, 2024. [specification](https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral) | R20 `PbrNeutralTonemapper` in `lib/color.slang`, CPU reference in `color_grading.cpp` (#79) |
| 🔧 | **ACEScct — A Quasi-Logarithmic Encoding of ACES Data** — Academy S-2016-001. [ACES documentation](https://docs.acescentral.com/) | R20 grading (contrast, lift / gamma / gain) in `lib/color.slang` and `color_grading.cpp` (#79) |
| 🔧 | **Chromatic adaptation** (von Kries in Bradford LMS) — Lindbloom; and **Planckian locus approximation** — Krystek, *Color Res. Appl.* 1985. [Lindbloom](http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html) | R20 white balance, `color_grading.cpp` `white_balance_matrix` (#79) |
| 🔧 | **Cube LUT Specification 1.0** — Adobe, 2013 (the `.cube` text format Resolve also writes). | R20 look LUTs: parser `src/asset/cube_lut.cpp` (#79) |
| ✅ | **Stochastic Screen-Space Reflections** — Stachowiak, SIGGRAPH 2015. [EA](https://www.ea.com/frostbite/news/stochastic-screen-space-reflections) | `assets/shaders/ssr_trace.slang`, `ssr_resolve.slang` (ray reuse), `ssr_temporal.slang`; `src/renderer/ssr_pass.cpp` |
| ✅ | **Sampling the GGX Distribution of Visible Normals** — Heitz, JCGT 2018. [JCGT](https://jcgt.org/published/0007/04/01/) | SSR ray directions: `lib/ssr_common.slang` (`ssr_sample_vndf`) |
| ✅ | **Hi-Z Screen-Space Cone-Traced Reflections** — Uludag, GPU Pro 5, 2014; with the min-Z traversal of **AMD FidelityFX SSSR**. [FidelityFX SSSR](https://gpuopen.com/fidelityfx-sssr/) | SSR Hi-Z march: `assets/shaders/ssr_trace.slang` (`hiz_march`) over R12's pyramid |
| ✅ | **Building an Orthonormal Basis, Revisited** — Duff, Burgess, Christensen, Hery, Kensler, Liani, Villemin, JCGT 2017. [JCGT](https://jcgt.org/published/0006/01/01/) | `lib/ssr_common.slang` (`ssr_basis`), `lib/particle_common.slang`, `src/renderer/particles.cpp` |

## Transparency, particles & VFX

| | Reading | In quat-3d |
|---|---|---|
| 🔧 | **Weighted Blended Order-Independent Transparency** — McGuire & Bavoil, JCGT 2013. [JCGT](https://jcgt.org/published/0002/02/09/) | `particle_draw.slang`, `particle_composite.slang` (R10.3 GPU particles, #26 in the merge train) |
| ✅ | **A Phenomenological Scattering Model for Order-Independent Transparency** — McGuire & Mara, I3D 2016. [PDF](https://research.nvidia.com/sites/default/files/pubs/2016-02_A-Phenomenological-Scattering/McGuire2016Transparency.pdf) | Coloured shadows through glass and blended casters: `shadow_material.slang` translucency map (#70) |
| 🔧 | **Holy Smoke! Faster Particle Rendering Using DirectCompute** — Thomas (AMD), AMD/Microsoft Developer Day, Stockholm 2014; follows his GDC 2014 talk *Compute-Based GPU Particle Systems*. The canonical GPU particle architecture: dead/alive lists, compute emit + simulate, depth-buffer collisions, GPU sort, tiled compute rendering. [slides](https://www.slideshare.net/slideshow/holy-smoke-faster-particle-rendering-using-direct-compute-by-gareth-thomas/35656271) · [GDC Vault](https://gdcvault.com/play/1020002/Advanced-Visual-Effects-with-DirectX) · [GPUParticles11 sample](https://gpuopen.com/learn/gpuparticles11-directx-11-sdk-sample/) | R10.3 GPU particles: compute emit / simulate / compact, depth collisions (#26) |
| 🔧 | **GPU-based particle simulation** — Turánszki (Wicked Engine blog), 2017. A readable end-to-end implementation with indirect-argument buffers. [wickedengine.net](https://wickedengine.net/2017/11/gpu-based-particle-simulation) | R10.3 (#26) |
| 🔧 | **Soft Particles** — Lorach (NVIDIA), 2007 whitepaper. Depth-fade where sprites meet geometry. [PDF](https://developer.download.nvidia.com/whitepapers/2007/SDK10/SoftParticles_hi.pdf) | Soft-particle fade in `particle_draw.slang` (#26) |
| 🗺 | **High-Speed, Off-Screen Particles** — Cantlay, GPU Gems 3 ch. 23 (2007). Low-resolution particle rendering to bound fill rate. [online](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles) | Particle fill-rate budget (not scoped on the ROADMAP yet) |
| 🗺 | **Programmable VFX with Unreal Engine's Niagara** — Wyeth Johnson (Epic), GDC 2018, the talk that introduced Niagara's modular emitter stack. [session page](https://www.unrealengine.com/events/gdc2018/programmable-vfx-with-unreal-engine-s-niagara) · [Niagara enhancements and samples (Epic tech blog, 2018)](https://www.unrealengine.com/en-US/tech-blog/niagara-enhancements-and-sample-overview) | R18 VFX graph (Niagara-class) |
| 🗺 | **Niagara documentation** — Epic. Systems, emitters, modules and parameters; the model R18 follows. [Overview of Niagara Effects](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-niagara-effects-for-unreal-engine) | R18 |
| 🗺 | **Visual Effect Graph manual** — Unity. The other production node-graph GPU VFX system; its contexts/blocks model is a useful comparison. [docs.unity3d.com](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.2/manual/toc.html) | R18 |
| 🗺 | **Curl-Noise for Procedural Fluid Flow** — Bridson, Hourihan, Nordenstam, SIGGRAPH 2007. Divergence-free turbulence for smoke and sparks without a simulation. [PDF](https://www.dneg.com/wp-content/uploads/2022/11/2007.pdf) · [ACM](https://doi.org/10.1145/1239451.1239497) | R18 update modules (turbulence; not in R18's scope list yet); cloud distortion (clouds v2) |
| 🗺 | **Real-Time Fluid Dynamics for Games** — Stam, GDC 2003 (stable fluids, with full C source). [Autodesk Research](https://www.research.autodesk.com/publications/real-time-fluid-dynamics-for-games/) | Local smoke/fire grids (beyond R18's current scope) |
| 🗺 | **Real-Time Simulation and Rendering of 3D Fluids** — Crane, Llamas, Tariq, GPU Gems 3 ch. 30 (2007). [online](https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids) | Local smoke/fire grids (beyond R18's current scope) |
| 🗺 | **Position Based Fluids** — Macklin & Müller, SIGGRAPH 2013; **Unified Particle Physics for Real-Time Applications** — Macklin, Müller, Chentanez, Kim, SIGGRAPH 2014. [PBF (dblp)](https://dblp.uni-trier.de/rec/journals/tog/MacklinM13.html) · [Unified (PDF)](https://matthias-research.github.io/pages/publications/flex.pdf) | Particle fluids (beyond R18's current scope) |

## Terrain, scattering & foliage

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Fast Hydraulic Erosion Simulation and Visualization on GPU** — Mei, Decaudin, Hu, Pacific Graphics 2007 (virtual pipes). [HAL](https://hal.archives-ouvertes.fr/inria-00402079) | `src/terrain/grid_erosion.cpp`, `include/engine/terrain/generator.hpp` |
| ✅ | **Fast Poisson Disk Sampling in Arbitrary Dimensions** — Bridson, SIGGRAPH 2007 sketch. [PDF](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf) | `src/scene/scatter.cpp` |
| ✅ | **Hash Functions for GPU Rendering** — Jarzynski & Olano, JCGT 2020 (pcg3d). [JCGT](https://jcgt.org/published/0009/03/02/) | `lib/grass_common.slang`, `include/engine/renderer/grass.hpp` |
| 🔧 | **Vegetation Procedural Animation and Shading in Crysis** — Sousa, GPU Gems 3 ch. 16 (2007). [online](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis) | Foliage wind (R14, #49; R13 wind field, #64) |
| 🗺 | **GPU-Based Procedural Placement in Horizon Zero Dawn** — van Muijden, GDC 2017. [Guerrilla](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn) | R14 biome placement |
| 🔧 | **Stochastic Simplification of Aggregate Detail** — Cook, Halstead, Planck & Ryu, SIGGRAPH 2007 (Pixar): keep a random subset of the elements of foliage / fur and scale the survivors to preserve area. [Pixar](https://graphics.pixar.com/library/StochasticSimplification) | `quat-cook --simplify` card thinning, `src/cook/mesh_simplify.cpp` (#91) |
| 🔧 | **Surface Simplification Using Quadric Error Metrics** — Garland & Heckbert, SIGGRAPH 1997; the edge-collapse metric behind meshoptimizer's simplifier (with attribute error). [CMU](https://www.cs.cmu.edu/~garland/quadrics/) | `quat-cook --simplify` solid meshes, `src/cook/mesh_simplify.cpp` (#91) |
| 🗺 | **Octahedral Impostors** — Brucks, 2018. [shaderbits.com](https://shaderbits.com/blog/octahedral-impostors) | R14 far-field trees |

## Procedural building, curves & geometry

Design: [`docs/design/cozy-builder.md`](../design/cozy-builder.md) (engine
items G18, R23, R9.5, G19, R24; the Cozy Builder game, G16).

| | Reading | In quat-3d |
|---|---|---|
| 🗺 | **Rendering Tiny Glades With Entirely Too Much Ray Marching** — Stachowiak (Pounce Light), Graphics Programming Conference 2024. GI, GPU-driven rendering, tilt-shift DoF, stable shadows under a moving sun. [video](https://www.youtube.com/watch?v=jusWW2pPnA0) · [GPC 2024 archive](https://graphicsprogrammingconference.com/archive/2024/) | Reference look for G16; R20 DoF, R12 CSM, R15 GI |
| 🗺 | **Tiny Glade Developers on Bevy, Proceduralism, Publishers & Cozy Games** — Opara & Stachowiak, 80.lv interview. Rule-authored, real-time (GPU) procedural generation; brush-painted paths. [80.lv](https://80.lv/articles/exclusive-tiny-glade-developers-discuss-bevy-proceduralism-publishers-cozy-games) · dev clips: [walls](https://80.lv/articles/anastasia-opara-s-procedural-wall-generator-upgraded), [glued buildings](https://80.lv/articles/this-upcoming-game-lets-you-procedurally-glue-buildings-together), [tower roofs](https://80.lv/articles/a-look-at-procedural-tower-roofs-in-tiny-glade), [windows](https://80.lv/articles/procedural-windows-in-pounce-light-s-tiny-glade-get-an-upgrade/) | G16 intent-vs-derived model and feature set |
| 🗺 | **Functional Symbiosis of Art Direction and Proceduralism** — Opara (EA SEED), CEDEC 2018. [PDF](https://media.contentapi.ea.com/content/dam/ea/seed/presentations/cedec-2018-functional-symbiosis-of-art-direction-and-proceduralism.pdf) | G16 hand-authored rule philosophy |
| 🗺 | **Organic Towns from Square Tiles** — Stålberg, IndieCade Europe 2019 (Townscaper's irregular grid and topology-driven module choice). [Game Developer summary](https://www.gamedeveloper.com/game-platforms/how-townscaper-works-a-story-four-games-in-the-making) | Considered for G16 and rejected (gridless); its idea survives in G16.2's junction rules on a curve graph |
| 🗺 | **A Novel Type of Skeleton for Polygons** — Aichholzer, Aurenhammer, Alberts, Gärtner, J.UCS 1995. The straight skeleton, the canonical roof over a polygon. [J.UCS](https://doi.org/10.3217/jucs-001-12-0752) | R23.1 straight skeleton; G16.3 roofs |
| 🗺 | **Straight Skeleton Implementation** — Felkel & Obdržálek, SCCG 1998 (the event algorithm; known to be incorrect in corner cases). [PDF](https://www.sthu.org/misc/SKRW14/papers/Felkel_1998_StraightSkeletonImplementation.pdf) | R23.1 |
| 🗺 | **On Implementing Straight Skeletons: Challenges and Experiences** — Eder, Held, Palfrader, SoCG 2020. Robustness and exact arithmetic. [LIPIcs](https://doi.org/10.4230/LIPIcs.SoCG.2020.38) | R23.1 robustness and tests |
| 🗺 | **Interactive Architectural Modeling with Procedural Extrusions** — Kelly & Wonka, ACM TOG 30(2), 2011. Weighted skeletons: gables, mixed pitches. [DOI](https://doi.org/10.1145/1944846.1944854) | R23.1 weights; G16.3 gable and mansard roofs |
| 🗺 | **Procedural Modeling of Buildings** — Müller, Wonka, Haegler, Ulmer, Van Gool, SIGGRAPH 2006 (CGA shape grammar). [DOI](https://doi.org/10.1145/1141911.1141931) | G16.3 façade split rules (bays, doors, windows) |
| 🗺 | **Instant Architecture** — Wonka, Wimmer, Sillion, Ribarsky, SIGGRAPH 2003 (split grammars). [DOI](https://doi.org/10.1145/882262.882324) | G16.3 façade rules |
| 🗺 | **Computation of Rotation Minimizing Frames** — Wang, Jüttler, Zheng, Liu, ACM TOG 27(1), 2008 (double reflection). [DOI](https://doi.org/10.1145/1330511.1330513) | G18.1 curve frames; R23.1 sweeps (rails, ivy) |
| 🗺 | **Parameterization and applications of Catmull-Rom curves** — Yuksel, Schaefer, Keyser, Computer-Aided Design 43(7), 2011 (centripetal Catmull-Rom). [project page](https://cemyuksel.com/research/catmullrom_param/) · [DOI](https://doi.org/10.1016/j.cad.2010.08.008) | G18.1 default spline kind |
| 🗺 | **An Algorithm for Automatically Fitting Digitized Curves** — Schneider, Graphics Gems (1990). | G18.3 stroke capture |
| 🗺 | **Algorithms for the Reduction of the Number of Points Required to Represent a Digitized Line or its Caricature** — Douglas & Peucker, The Canadian Cartographer 10(2), 1973 (with Ramer 1972: polyline simplification). | G18.3 stroke capture; G16.3 footprint simplification |
| 🗺 | **Simulation of Wrinkled Surfaces Revisited** — Mikkelsen, 2008 (MikkTSpace tangents); the reference implementation is [mmikk/MikkTSpace](https://github.com/mmikk/MikkTSpace). | R23.1 mesh builder tangents |
| 🗺 | **An Ivy Generator** — Luft, 2007 (growth by direction, randomness and adhesion). [project page](https://graphics.uni-konstanz.de/~luft/ivy_generator/) | G16.4 ivy |
| 🗺 | **Screen Space Decals in Warhammer 40,000: Space Marine** — Kim, SIGGRAPH 2012 talks. | R24 decals |
| 🗺 | **Clipper2** — Johnson (polygon clipping and offsetting, Boost licence) and **earcut.hpp** — Mapbox (ear-clipping triangulation, ISC). [Clipper2](https://github.com/AngusJohnson/Clipper2) · [earcut.hpp](https://github.com/mapbox/earcut.hpp) | R23.1 polygon operations |

## Seasons, snow & ambient life

Design: [`docs/design/cozy-builder.md` §4.14](../design/cozy-builder.md#414-seasons-r25) (R25) and §5.16 (G16.10).

| | Reading | In quat-3d |
|---|---|---|
| 🗺 | **Real-time Rendering of Accumulated Snow** — Ohlsson & Seipel, SIGRAD 2004. Exposure from a top-down depth map plus slope-based coverage. [PDF](https://ep.liu.se/ecp/013/007/ecp01307.pdf) | R25.3 precipitation occlusion map and snow coverage |
| 🗺 | **Real-time accumulation of occlusion-based snow** — Reynolds, Laycock & Day, The Visual Computer 2015. [DOI](https://doi.org/10.1007/s00371-014-0995-5) | R25.3 |
| 🗺 | **Computer Modelling of Fallen Snow** — Fearing, SIGGRAPH 2000 (offline accumulation and stability; the physical reference). [SIGGRAPH history](https://history.siggraph.org/?p=118252) | R25.3 coverage rules |
| 🗺 | **Real-Time Rendering of Seasonal Influenced Trees** — Braitmaier, Diepstraten & Ertl, Theory and Practice of Computer Graphics 2004 (leaf pigments in shaders). | R25.2 `ColourRamp` |
| 🗺 | **Realistic Simulation of Seasonal Variant Maples** — Zhou, Dong & Mei (chlorophyll / carotenoid / anthocyanin over time). [HAL](https://hal.archives-ouvertes.fr/inria-00521833) | R25.2 ramp authoring |
| 🗺 | **Flocks, Herds, and Schools: A Distributed Behavioral Model** — Reynolds, SIGGRAPH 1987 (boids). [DOI](https://doi.org/10.1145/37401.37406) | G16.9 birds |

## Cameras, tools & game architecture

| | Reading | In quat-3d |
|---|---|---|
| 🗺 | **Critically Damped Ease-In/Ease-Out Smoothing** — Lowe, Game Programming Gems 4 (2004). See also Holden's *Spring-It-On* under Animation. | G15.1 camera rig smoothing |
| 🗺 | **50 Game Camera Mistakes** — Nesky, GDC 2014. [GDC Vault](https://www.gdcvault.com/play/1020460/50-Camera) | G15 camera rigs |
| 🗺 | **Real-Time Cameras** — Haigh-Hutchinson, Morgan Kaufmann 2009. | G15 camera rigs |
| 🗺 | **Semantic Versioning 2.0.0** — Preston-Werner. [semver.org](https://semver.org/spec/v2.0.0.html) | P8 SDK versioning policy |
| 🗺 | **CMake: Importing and Exporting Guide** — Kitware (exported targets, package config and version files). [cmake.org](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html) | P8 `find_package(quat3d CONFIG)` |
| 🗺 | **Game Programming Patterns: Command** — Nystrom, 2014. [online](https://gameprogrammingpatterns.com/command.html) · and **Design Patterns** — Gamma, Helm, Johnson, Vlissides, 1994 (Command, Memento). | E1 `core::CommandStack`; G16.6 undo/redo |

## Animation, physics & navigation

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Spring-It-On: The Game Developer's Spring-Roll-Call** — Holden, 2021 (critically damped springs). [theorangeduck.com](https://www.theorangeduck.com/page/spring-roll-call) | `src/animation/ik/foot_placement.cpp` |
| 🗺 | **FABRIK: A fast, iterative solver for the Inverse Kinematics problem** — Aristidou & Lasenby, Graphical Models 2011. [project page](https://www.andreasaristidou.com/FABRIK) | Multi-bone IK chains (today: analytic two-bone IK in `include/engine/animation/two_bone_ik.hpp`) |
| 🗺 | **Motion Matching and The Road to Next-Gen Animation** — Clavet, GDC 2016. [GDC Vault](https://www.gdcvault.com/play/1022985/Motion-Matching-and-The-Road) | Animation track |
| 🗺 | **Learned Motion Matching** — Holden, Kanoun, Perepichka, Popa, SIGGRAPH 2020. [theorangeduck.com](https://www.theorangeduck.com/page/learned-motion-matching) | Animation track |
| 🗺 | **XPBD: Position-Based Simulation of Compliant Constrained Dynamics** — Macklin, Müller, Chentanez, MIG 2016. [PDF](https://matthias-research.github.io/pages/publications/XPBD.pdf) | Cloth (G-track) |
| ✅ | **Jolt Physics** documentation — Rouwé. [jrouwe.github.io/JoltPhysics](https://jrouwe.github.io/JoltPhysics/) | `src/physics/` |
| ✅ | **Recast & Detour** — Mononen. [GitHub](https://github.com/recastnavigation/recastnavigation) | Navigation meshes and pathfinding |
| 🔧 | **Optimized Spatial Hashing for Collision Detection of Deformable Objects** — Teschner, Heidelberger, Müller, Pomeranets, Gross, VMV 2003. A uniform grid hashed into a fixed-size table, for neighbour queries without a grid the size of the world. [PDF](https://cgweb.informatik.uni-freiburg.de/publications/2003_VMV_collisionDetectionHashing.pdf) | Nav agent separation neighbours, `src/scene/nav_separation.cpp` (#94) |
| 🔧 | **Particle Simulation using CUDA** — Green (NVIDIA), 2010. Sort particles by cell hash and keep a cell-start table, instead of per-cell lists. [PDF](https://developer.download.nvidia.com/assets/cuda/files/particles.pdf) | The same grid's flat layout, built with a counting sort (#94) |
| 🔧 | **The Art of Computer Programming, Vol. 3: Sorting and Searching**, §6.4 (multiplicative "Fibonacci" hashing) — Knuth, 2nd ed., 1998. [book page](https://www-cs-faculty.stanford.edu/~knuth/taocp.html) | Cell-key hash in `src/scene/nav_separation.cpp` (#94) |

## Networking

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Fix Your Timestep!** — Fiedler (Gaffer On Games), 2004. Fixed-step simulation with an accumulator. [gafferongames.com](https://gafferongames.com/post/fix_your_timestep/) | Jolt steps on a fixed 1/120 s accumulator (`src/physics/physics_world.cpp`); `src/core/sim_clock.cpp` drops hitches over 0.25 s and slices long frames into physics-budget steps |
| ✅ | **Networking for Game Programmers** and **Networked Physics** (series) — Fiedler. UDP, reliability, snapshot interpolation, state sync. [gafferongames.com](https://gafferongames.com/) · [Networked Physics](https://gafferongames.com/categories/networked-physics/) | Snapshots in `src/net/replication.cpp` (clients smooth exponentially toward the latest snapshot rather than buffering and interpolating between two); UDP and reliability come from GameNetworkingSockets (`src/net/transport.cpp`) |
| ✅ | **Fast-Paced Multiplayer** (series) — Gambetta. Client prediction, server reconciliation, entity interpolation, lag compensation. [gabrielgambetta.com](https://www.gabrielgambetta.com/client-server-game-architecture.html) | Prediction + reconciliation: `src/net/prediction.cpp`, `character_step.cpp` (no lag compensation yet) |
| ✅ | **Latency Compensating Methods in Client/Server In-game Protocol Design and Optimization** — Bernier (Valve), GDC 2001; and **Source Multiplayer Networking**. [Bernier](https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization) · [Source networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking) | Client prediction in `src/net/prediction.cpp`; server-side lag compensation not implemented |
| 🗺 | **Overwatch Gameplay Architecture and Netcode** — Timothy Ford (Blizzard), GDC 2017. ECS plus deterministic prediction and rollback. [GDC Vault](https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and) | Net + ECS design |
| 🗺 | **I Shot You First: Networking the Gameplay of Halo: Reach** — Aldridge (Bungie), GDC 2011. [GDC Vault](https://www.gdcvault.com/play/1014346/I-Shot-You-First-Networking) · [video (GDC YouTube)](https://www.youtube.com/watch?v=h47zZrqjgLc) | Replication priorities |
| 🗺 | **8 Frames in 16ms: Rollback Networking in Mortal Kombat and Injustice 2** — Stallone (NetherRealm), GDC 2018. [GDC Vault](https://www.gdcvault.com/play/1025471/8-Frames-in-16ms-Rollback) | Rollback (A10.4 checkpoints) |
| ✅ | **Quake 3 Source Code Review: Network Model** — Sanglard, 2012. Delta-compressed snapshots, in readable form. [fabiensanglard.net](https://fabiensanglard.net/quake3/network.php) | `src/net/replication.cpp` sends only entities that moved since the last snapshot plus periodic reliable keyframes: simpler than Quake 3's per-client delta against the last acknowledged snapshot |

## Water

| | Reading | In quat-3d |
|---|---|---|
| 🗺 | **Simulating Ocean Water** — Tessendorf, SIGGRAPH course notes, 2001 (FFT oceans; 2004 revision linked). [PDF](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2004.pdf) | Water track (see tech radar) |
