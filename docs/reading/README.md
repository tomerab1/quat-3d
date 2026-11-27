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
4. **Physically Based Rendering: From Theory to Implementation, 4th ed.** —
   Pharr, Jakob, Humphreys. Free online; the theory behind microfacets,
   sampling and volumes. [pbr-book.org](https://pbr-book.org/4ed/contents)
5. **Advances in Real-Time Rendering** course archive (SIGGRAPH, 2006–today).
   Where studios publish what they ship. [advances.realtimerendering.com](https://advances.realtimerendering.com/)
   — and Stephen Hill's archive of the PBR shading courses:
   [blog.selfshadow.com/publications](https://blog.selfshadow.com/publications/)

---

## Engine architecture & performance

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **FrameGraph: Extensible Rendering Architecture in Frostbite** — O'Donnell, GDC 2017. [GDC Vault](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in) | Render graph: `src/rhi/render_graph.cpp`, `include/engine/rhi/render_graph.hpp` |
| ✅ | **ECS back and forth** (series) — Caini (author of entt), 2019. [part 1](https://skypjack.github.io/2019-02-14-ecs-baf-part-1/) | ECS on entt: `src/scene/` |
| 🔧 | **Tracy profiler manual** — Taudul. [github.com/wolfpld/tracy](https://github.com/wolfpld/tracy) | R4 profiling (in progress) |
| 🗺 | **Data-Oriented Design and C++** — Acton, CppCon 2014. [video](https://www.youtube.com/watch?v=rX0ItVEVjHc) | R4.5 data-oriented CPU pass |
| 🗺 | **Data-Oriented Design** (book) — Fabian. Free online. [dataorienteddesign.com](https://www.dataorienteddesign.com/dodbook/) | R4.5 |
| 🗺 | **What Every Programmer Should Know About Memory** — Drepper, 2007. [PDF](https://www.akkadia.org/drepper/cpumemory.pdf) | R4.5 (caches, alignment, false sharing) |
| ✅ | **Vulkan Guide** (Khronos) and **Synchronization Examples**. [docs.vulkan.org/guide](https://docs.vulkan.org/guide/latest/index.html) · [sync examples](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples) | `src/rhi/` (sync2, dynamic rendering, descriptor buffer) |
| 🗺 | **GPU-Driven Rendering Pipelines** — Haar & Aaltonen, SIGGRAPH 2015. [PDF](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf) | R5 GPU-driven rendering, R6 GPU culling |
| 🗺 | **Nanite: A Deep Dive** — Karis, Stubbe, Wihlidal, SIGGRAPH 2021. [PDF](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) | R19 virtual geometry |

## Materials & shading (PBR)

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Microfacet Models for Refraction through Rough Surfaces** — Walter et al., EGSR 2007 (the GGX distribution). [PDF](https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.pdf) | `assets/shaders/lighting.slang`, `lib/pbr.slang` |
| ✅ | **Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs** — Heitz, JCGT 2014 (height-correlated Smith). [JCGT](https://jcgt.org/published/0003/02/03/) | `lib/pbr.slang` |
| ✅ | **Physically-Based Shading at Disney** — Burley, SIGGRAPH 2012. [PDF](https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf) | Material parameterisation |
| ✅ | **Real Shading in Unreal Engine 4** — Karis 2013 (see *Where to start*) | Split-sum IBL, `lib/pbr.slang` |
| ✅ | **Filtering Distributions of Normals for Shading Antialiasing** — Kaplanyan, Hill, Patney, Lefohn, HPG 2016; refined by Tokuyoshi & Kaplanyan, I3D 2019. [NVIDIA Research](https://research.nvidia.com/publication/2016-06_filtering-distributions-normals-shading-antialiasing) | Geometric specular AA, `lib/pbr.slang` |
| 🔧 | **Revisiting Physically Based Shading at Imageworks** — Kulla & Conty, SIGGRAPH 2017 (energy-conserving multiple scattering). [course page](https://blog.selfshadow.com/publications/s2017-shading-course/) | R21 lighting realism (#80) |
| 🔧 | **A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting** — Fdez-Agüera, JCGT 2019. [JCGT](https://jcgt.org/published/0008/01/03/) | R21 (#80) |

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
| 🔧 | **Hashed Alpha Testing** — Wyman & McGuire, I3D 2017. [NVIDIA Research](https://research.nvidia.com/labs/rtr/publication/wyman2017hashed) | Alpha MASK cut-outs (#70) |
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

## Anti-aliasing, post-processing & camera

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **High Quality Temporal Supersampling** — Karis, SIGGRAPH 2014. [course page](https://advances.realtimerendering.com/s2014/) | `assets/shaders/taa.slang` |
| ✅ | **An Excursion in Temporal Supersampling** — Salvi, GDC 2016 (variance clipping). [PDF](https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf) | `taa.slang` |
| ✅ | **Temporal AA and the Quest for the Holy Trail** — López, 2022. Approachable TAA overview. [blog](https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/) | `taa.slang`; TAAU upscaler (R11) |
| ✅ | **Next Generation Post Processing in Call of Duty: Advanced Warfare** — Jimenez, SIGGRAPH 2014 (bloom, motion blur, DoF, interleaved gradient noise). [iryoku.com](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/) | `bloom_down.slang`; R20 DoF & motion blur (#79) |
| ✅ | **ACES Filmic Tone Mapping Curve** — Narkowicz, 2016. [blog](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/) | `assets/shaders/tonemap.slang` |
| 🔧 | **AgX** — Sobotka's view transform, and **Minimal AgX implementation** (iolite). [AgX](https://github.com/sobotka/AgX) · [minimal implementation](https://iolite-engine.com/blog_posts/minimal_agx_implementation) | R20 (#79) |
| 🔧 | **Stochastic Screen-Space Reflections** — Stachowiak, SIGGRAPH 2015. [EA](https://www.ea.com/frostbite/news/stochastic-screen-space-reflections) | R21 SSR (#80) |

## Transparency & particles

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Weighted Blended Order-Independent Transparency** — McGuire & Bavoil, JCGT 2013. [JCGT](https://jcgt.org/published/0002/02/09/) | `particle_draw.slang`, `particle_composite.slang` |
| 🔧 | **A Phenomenological Scattering Model for Order-Independent Transparency** — McGuire & Mara, I3D 2016. [PDF](https://research.nvidia.com/sites/default/files/pubs/2016-02_A-Phenomenological-Scattering/McGuire2016Transparency.pdf) | Glass / transmissive shadows (#70) |

## Terrain, scattering & foliage

| | Reading | In quat-3d |
|---|---|---|
| ✅ | **Fast Hydraulic Erosion Simulation and Visualization on GPU** — Mei, Decaudin, Hu, Pacific Graphics 2007 (virtual pipes). [HAL](https://hal.archives-ouvertes.fr/inria-00402079) | `src/terrain/grid_erosion.cpp`, `include/engine/terrain/generator.hpp` |
| ✅ | **Fast Poisson Disk Sampling in Arbitrary Dimensions** — Bridson, SIGGRAPH 2007 sketch. [PDF](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf) | `src/scene/scatter.cpp` |
| ✅ | **Hash Functions for GPU Rendering** — Jarzynski & Olano, JCGT 2020 (pcg3d). [JCGT](https://jcgt.org/published/0009/03/02/) | `lib/grass_common.slang`, `include/engine/renderer/grass.hpp` |
| 🔧 | **Vegetation Procedural Animation and Shading in Crysis** — Sousa, GPU Gems 3 ch. 16 (2007). [online](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis) | Foliage wind (R14, #49; R13 wind field, #64) |
| 🗺 | **GPU-Based Procedural Placement in Horizon Zero Dawn** — van Muijden, GDC 2017. [Guerrilla](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn) | R14 biome placement |
| 🗺 | **Octahedral Impostors** — Brucks, 2018. [shaderbits.com](https://shaderbits.com/blog/octahedral-impostors) | R14 far-field trees |

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

## Water

| | Reading | In quat-3d |
|---|---|---|
| 🗺 | **Simulating Ocean Water** — Tessendorf, SIGGRAPH course notes, 2001 (FFT oceans; 2004 revision linked). [PDF](https://jtessen.people.clemson.edu/reports/papers_files/coursenotes2004.pdf) | Water track (see tech radar) |
