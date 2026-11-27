# Real-Time Foliage and Vegetation Rendering (2018–2026): Technique Catalogue for quat-3d

> Method note: the cloud session blocked direct fetches of most primary hosts (gdcvault.com,
> dev.epicgames.com, guerrilla-games.com, shaderbits.com, developer.nvidia.com, 80.lv). Findings
> below come from web-search result extracts of those pages plus the primary URLs themselves. Each
> claim is cited to the page the extract came from. Anything I could not confirm is under Gaps or
> marked as an inference. Target-engine context comes from `ROADMAP.md`. R9.1 instanced draws and
> R9.2 `InstancedMesh` + `Scatter` are done. R9.3 GPU grass, R9.4 instance culling + LOD, R5
> GPU-driven, R6 GPU culling and R10.2 GPU terrain synthesis are still open.

## Q1. Tree/plant wind animation: hierarchical, pivot-painted and skeletal wind, and wind fields that react to characters and explosions

### Takeaway
Production wind has three tiers. (1) Cheap per-vertex procedural bending: Crysis main bending plus detail bending, and SpeedTree's ripple and branch levels. (2) Hierarchical rigid-pivot bending driven by baked per-element pivot and parent data: Pivot Painter 2, SpeedTree's 4 wind levels. (3) Skeletal or joint-driven plants sampling a shared, simulated wind field: Ghost of Tsushima, God of War, and UE 5.7 Nanite Foliage "Dynamic Wind", which simulates bones on the GPU instead of using vertex offset. In every shipped AAA example, one global wind field (noise gusts, sometimes a GPU fluid or vorticle simulation) drives grass, trees, particles and cloth together, which keeps motion coherent.

### Cited Findings
**Foundational procedural bending (background, 2008)**
- Crysis (Tiago Sousa, Crytek) splits vegetation motion into "main bending" of the whole plant and "detail bending" of leaves and edges. It replaces sine waves with cheaper triangle waves smoothed by cubic interpolation, and warns that too much bending breaks the illusion. GPU Gems 3 ch. 16, pp. 373–385 — [NVIDIA GPU Gems 3 ch.16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)
- The same chapter frames the shading and animation budget around "thousands of different vegetation objects ... often covering the entire screen" — [NVIDIA GPU Gems 3 ch.16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)
- Companion chapter: GPU Gems 3 ch. 6, "GPU-Generated Procedural Wind Animations for Trees" — [NVIDIA GPU Gems 3 ch.6](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees)

**SpeedTree (licensed middleware; shipped in The Witcher 3 / REDengine 3 and many others)**
- CD Projekt Red integrated SpeedTree for Games into REDengine 3. SpeedTree's runtime handles "loading, culling, level-of-detail, lighting, and wind effects for entire forests" — [GamingBolt SpeedTree interview](https://gamingbolt.com/speedtree-interview-admiring-the-forest-for-the-virtual-trees)
- SpeedTree branch motion uses five groups of properties: one for whole-model motion (for example a pine rocking) and four for the four discrete wind levels — [SpeedTree Modeler: Wind (Games)](https://docs.unity3d.com/speedtree-modeler/manual/wind-games.html)
- "Ripple" moves leaf and frond vertices along their vertex normals using a noise pattern. Branch wind "can be the most expensive wind component in a SpeedTree vertex shader", so the second branch level should be enabled only where needed. An `m_bLodFade` flag can fade detailed near wind into global-only motion at distance. A "wind independence" parameter de-synchronises instances (0 = identical, 1 = unsynchronised) — [SpeedTree Runtime SDK: Wind shader configuration](https://docs.unity3d.com/speedtree-runtime-sdk/manual/wind-shader-configuration.html)
- In VFX mode, gusts add strength on top of nominal wind, with variance and rise/fall scalars — [SpeedTree Modeler: Wind (VFX)](https://docs.unity3d.com/speedtree-modeler/manual/wind-vfx.html)
- Witcher 3's wind drew criticism as well as praise. Players found the "pulsating" overdone, and mods exist to reduce it, which shows how much art tuning matters — [PC Gamer, "Why I love The Witcher 3's wobbly bushes"](https://www.pcgamer.com/the-witcher-3s-wobbly-bushes); [Kotaku](https://kotaku.com/the-witcher-3s-wind-is-beautiful-1797285984)

**Pivot Painter 2 (Epic; UE4/UE5)**
- A MAXScript bakes per-element pivot positions and rotation data into textures that Unreal's shader system reads. Leaves and branches are modelled as separate elements, with the X axis along each element's length, and linked like a rig — [Epic docs: Pivot Painter Tool 2.0](https://dev.epicgames.com/documentation/unreal-engine/pivot-painter-tool-2.0-in-unreal-engine); [UE 4.27 docs](https://docs.unrealengine.com/4.27/AnimatingObjects/PivotPainter/PivotPainter2)
- Per-element data needed to rebuild the hierarchy in-shader: pivot position, parent index, forward direction and branch extent length. Children rotate about their parent's pivot — [tianc377: Tree wind with PivotPainter2 (Houdini/Unreal)](https://tianc377.github.io/posts/TreeWindAnimationWithUnrealPivotPainter)
- Third-party tools drive up to 4 hierarchy levels (trunk/branch/twig/leaf). One vendor claims the stock SpeedTree importer supports only 2-level wind (vendor claim) — [Pivot Painter Lab forum post](https://forums.unrealengine.com/t/eqlabs-pivot-painter-lab-one-click-foliage-wind/2841545); [IGToolsPP SpeedTree→PP2](https://forums.unrealengine.com/t/inu-games-igtoolspp-speedtree-to-pivot-painter-2-0/2417258)

**Ghost of Tsushima (Sucker Punch, 2020) / Ghost of Yōtei (2025)**
- Plants are "all rigged with joints that respond to the local wind speed". Trunks and branches have separate controls. The same gust noise drives trees and grass so they move together, and rope and cloth read the same wind inputs — [PlayStation Blog, Matt Vainio, VFX of Ghost of Tsushima](https://blog.playstation.com/?p=345372)
- Up to 60,000 individual leaf particles can be blowing in the wind at once — [PlayStation Blog](https://blog.playstation.com/?p=345372)
- GDC 2021, Bill Rockenbeck, "Blowing from the West: Simulating Wind in Ghost of Tsushima": a vorticle-based wind simulation that favoured "volume over accuracy" (heuristics and hacks) to simulate huge numbers of wind-affected particles on a small slice of PS4 compute — [GDC Vault](https://gdcvault.com/play/1027350/Blowing-from-the-West-Simulating); [Game Developer: "Using vorticles to simulate wind"](https://gamedeveloper.com/design/using-vorticles-to-simulate-wind-in-i-ghost-of-tsushima-i-)
- Ghost of Yōtei: grass and small plants bend with wind and with character movement. Weapon swings write a "cut buffer" that spawns particles, so most grass, flowers and small plants can be cut — [PlayStation Blog, Ghost of Yōtei tech deep dive](https://blog.playstation.com/?p=411201)

**God of War (Santa Monica, GDC 2019): two talks**
- Rupert Renard, "Wind Simulation in God of War": a full 3D GPU fluid simulation for wind, with the speed techniques that worked and failed — [GDC Vault 1025972](https://gdcvault.com/play/1025972/Wind-Simulation-in-God-of)
- "Interactive Wind and Vegetation in God of War" (Sean Feeley per 80.lv): dynamic, spatially varying 3D wind; interactive *boneless* tree and leaf sway; ground-vegetation character interaction — [GDC Vault 1026036](https://gdcvault.com/play/1026036/Interactive-Wind-and-Vegetation-in); [80.lv summary](https://80.lv/articles/santa-monica-studio-showed-how-interactive-wind-and-vegetation-were-made-for-god-of-war)

**Horizon Zero Dawn (Guerrilla, GDC 2018)**
- Gilbert Sanders, "Between Tech and Art: The Vegetation of Horizon Zero Dawn": covers polygon structure, leaf translucency and tree movement in wind — [Game Developer video post](https://gamedeveloper.com/art/video-how-guerrilla-crafted-the-lush-vegetation-of-i-horizon-zero-dawn-i-); [GDC Vault 1025530](https://www.gdcvault.com/play/1025530)
- A global wind force field sits at the base of the vegetation simulation. It is attached to the player or camera so resolution is highest near the player. This comes from an unofficial fan-hosted transcript — [mirror transcript](https://wiki.kwonsh.duckdns.org/Between-Tech-and-Art--The-Vegetation-of-/Between-Tech-and-Art--The-Vegetation-of-.html)

**UE 5.7 Nanite Foliage + Dynamic Wind (experimental, 2025)**
- Wind no longer uses World Position Offset. The Dynamic Wind plugin simulates the bones of skeletal plant meshes on the GPU, so Nanite keeps optimal (tight) cluster bounds. It requires SM6 and Nanite Foliage. Without Nanite Foliage, Procedural Vegetation Editor (PVE) meshes fall back to non-Nanite rendering and Dynamic Wind skips them — [Epic docs: Dynamic Wind for Nanite Foliage](https://dev.epicgames.com/documentation/unreal-engine/dynamic-wind-for-nanite-foliage-in-unreal-engine); [Epic docs: Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)
- Community-reported limitation (unverified against Epic docs): assembly parts are rigid transforms that follow bones, so vertices are not skinned — [UE forum: PVE with skeleton meshes and Wind Transform provider](https://forums.unrealengine.com/t/ue-57-new-procedural-vegetation-using-skeleton-meshes-and-wind-transform-provider/2677852); [UE forum: Nanite Foliage character interaction](https://forums.unrealengine.com/t/ue5-7-nanite-foliage-character-interaction-support-and-future-direction/2698270)
- A user reports severe game-thread slowdown with Megaplants + Nanite Foliage at scale in 5.7.2 — [UE forum](https://forums.unrealengine.com/t/severe-game-thread-slowdown-with-megaplants-nanite-foliage-at-scale-ue-5-7-2/2698484/7)

### Inferences
- **Fit for quat-3d: one shared wind field.** Add a `WindFieldPass` compute pass early in the render graph (quick win). It writes a camera-centred, low-resolution wind texture: either 2D (XZ velocity plus gust strength) or a small 3D volume (for example 32×16×32 clipmap texels). It layers a global direction, scrolling gust noise and local "wind emitters" (explosions, characters, helicopters) taken from a small SSBO. Grass (R9.3), tree and bush vertex shaders, GPU particles (R10.3) and cloth all sample it. This copies Tsushima's single gust source and HZD's camera-attached field. Upgrade path (long-term): replace the noise with a semi-Lagrangian fluid step (God of War) or vorticles (Tsushima). Both are pure compute and fit the render graph.
- **Tree and bush bending tiers for quat-3d:**
  - *Quick win:* Crysis-style main bending (height-weighted sway about the instance origin) plus detail bending (leaf flutter phase from a vertex attribute or hashed position). Use a per-instance phase from the existing seeded `Scatter` RNG, as in SpeedTree's "independence". This works directly on `InstancedMesh` with no asset-pipeline change, using vertex colours or a UV channel from glTF. Cost: a few dozen VS ALU ops per vertex.
  - *Medium:* Pivot-Painter-style hierarchical bending (2–4 levels). The cooker bakes per-element `{pivot, parent, axis, extent}` into a per-mesh SSBO addressed by a vertex attribute, read via buffer device address (the renderer already reads per-instance transforms via BDA, per R9.1). This gives proper branch-on-branch motion without skinning. Plain glTF has no slot for this metadata, so it needs either a cooker step that derives elements from connected components or a glTF extras convention.
  - *Long-term:* Skeletal wind, as in Tsushima and Nanite Dynamic Wind. A compute pass simulates per-instance bone springs against the wind field, then reuses the existing GPU skinning compute or a rigid-part transform (Nanite assemblies). Only near instances (LOD0) should get this. Far instances fall back to tier 1, following SpeedTree's `m_bLodFade` idea.
- **Rules conflict and correctness:** CLAUDE.md says "Never write skinning in the vertex shader". Procedural wind offsets are not skinning, but the team should state that VS wind offset is allowed for foliage (or route it through compute). Two correctness requirements: (a) the shadow pass must apply identical wind offsets, or self-shadow swims; (b) TAA needs per-vertex motion vectors that include *previous-frame* wind, so wind time and field must be double-buffered (frames-in-flight = 2 already). Nanite moved away from WPO partly for bounds reasons. quat-3d has the same issue: per-instance culling bounds must be padded by the maximum wind displacement.

### Gaps
- I could not read the Tsushima wind/grass slides, the God of War slides, or the HZD vegetation slides directly (GDC Vault is members-only or was unreachable). Specific formulas (spring constants, vorticle counts, field resolutions) are unverified.
- I found no primary CD Projekt Red technical talk on Witcher 3 foliage wind. A mirror titled "Large Scale Animated Foliage in The Witcher" exists, but I could not verify its provenance.
- Exact per-vertex channel conventions in Crysis (which vertex-colour channel holds stiffness and which holds phase) were not verifiable from extracts.
- No source found for Horizon Forbidden West's wind implementation specifically.

## Q2. LOD for vegetation: discrete LOD, cross-fade/dither, billboard and octahedral impostors, Nanite foliage and voxels, alpha-test cost, alpha-to-coverage, masked geometry in visibility buffers

### Takeaway
The state of the art moved from "alpha-tested cards + discrete LOD + billboards" toward "more opaque triangles + cluster LOD + volumetric/voxel far field". Epic found opaque high-poly foliage faster than masked under Nanite. UE 5.7 adds voxelised far LOD (clusters of 4×4×4 voxel bricks) for forests. For a non-Nanite Vulkan engine, the proven stack is: GPU-culled discrete LODs with dithered cross-fade resolved by TAA, then octahedral impostors at range, with alpha mip coverage preservation and hashed alpha for stability. Cluster/meshlet foliage with mesh shaders is the long-term path.

### Cited Findings
**Alpha-test vs opaque geometry (Nanite / Fortnite, UE 5.1)**
- Masked materials in Nanite add base-pass cost because barycentrics are recomputed per pixel, and transparent texels add overdraw. Fortnite Chapter 4 content "was usually faster without masked materials", using more triangles and opaque materials, especially for trees and grass. Programmable raster (WPO, pixel depth offset, masked) arrived in UE 5.1 as beta — [Epic Tech Blog: Bringing Nanite to Fortnite BR Ch.4](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)
- "Preserve Area" (UE 5.1) fixes canopies thinning or going bare under simplification. It redistributes lost area by dilating open boundary edges. It is "most useful for foliage where leaves tend to become disjoint triangles and quads" and should be enabled only on foliage meshes that show the problem — [Epic Tech Blog (Fortnite Ch.4)](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4); [Epic docs: Nanite technical details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details)
- User report (unverified): in 5.7 this became a "Shape Preservation" dropdown with "Preserve Area" and "Voxelize". Dark spikes with Preserve Area on dense foliage were reported in 5.2 — [UE forum: Preserve Area bug](https://forums.unrealengine.com/t/preserve-area-bug-any-solutions/1519543); [UE forum: Nanite trees issue](https://forums.unrealengine.com/t/nanite-trees-foliage-rendering-issue/2746491)

**Nanite Foliage voxel LOD (UE 5.7, experimental)**
- Distant trees switch to Nanite voxels, a new automatic LOD form. Voxel clusters are binned separately from triangle clusters and rasterized with a special shader permutation. The builder groups voxels into clusters of at most 128 4×4×4 voxel bricks — [Epic docs: Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage); [80.lv overview](https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7)
- The Witcher 4 UE 5.6 tech demo (State of Unreal, June 2025) ran on a base PS5 at 60 fps. Nanite Foliage was the headline feature for forests and grass, described as "a fast and memory efficient way to achieve gorgeous foliage density"; public availability was targeted for 5.7 — [CD PROJEKT RED press release](https://press.cdprojektred.com/news/1778/cd-projekt-red-and-epic-games-present-the-witcher-4-unreal-engine-5-tech-demo-at-the-state-of-unreal-2025); [PC Gamer](https://www.pcgamer.com/hardware/the-witcher-4s-leafy-glory-is-all-down-to-epics-nanite-foliage-and-largely-so-is-the-fact-it-can-run-ray-traced-at-60-fps-on-a-lowly-ps5)
- UE 5.7 also adds an experimental Procedural Vegetation Editor (PVE) plugin for growing and shaping "Nanite-ready foliage" in-editor — [UE 5.7 Preview forum post](https://forums.unrealengine.com/t/unreal-engine-5-7-preview/2658958)

**Deferred texturing for foliage (Horizon Forbidden West, GDC 2022)**
- James McLaren, "Adventures with Deferred Texturing in Horizon Forbidden West": a loosely tiled deferred texturing system speeds up foliage and alpha-tested geometry — [GDC Vault 1027553](https://www.gdcvault.com/play/1027553/contactUs); [Guerrilla publication page](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west)
- The technique did not ship on PC. Nixxes used a traditional G-buffer because it was a PlayStation-specific PS4 foliage optimisation with no easy PC equivalent (Digital Foundry interview repost) — [forum repost of DF interview](https://gaminglatest.com/threads/horizon-forbidden-west-pc-tech-interview-with-nixxes-and-guerrilla-games.118617)
- Alpha-tested trees are hard to ray trace, even for shadows, which was cited as a reason RT was dropped for HFW PC — [Wccftech](https://wccftech.com/horizon-forbidden-west-ray-tracing-support/); [Igor's Lab](https://igorslab.de/en/horizon-forbidden-west-in-ultra-hd-on-living-room-pc-solid-console-port-doesnt-need-expensive-gpu-heavyweights/3)

**Alpha test quality: hashed alpha and alpha-to-coverage**
- Hashed Alpha Testing (Wyman & McGuire, I3D 2017) addresses alpha-mapped geometry vanishing with distance. A hash picks a per-pixel threshold, which gives stable noise and less temporal flicker. The paper also covers application to alpha-to-coverage and screen-door — [paper PDF](https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf); [project page](https://research.nvidia.com/labs/rtr/publication/wyman2017hashed); follow-up [Improved Alpha Testing Using Hashed Sampling (2019)](https://research.nvidia.com/labs/rtr/publication/wyman2019improved)
- Alpha-to-coverage is sub-pixel screen-door transparency suited to dense foliage and grass. It needs MSAA — [Wikipedia: Alpha to coverage](https://en.wikipedia.org/wiki/Alpha_to_coverage); Ben Golus, "Anti-aliased Alpha Test: The Esoteric Alpha To Coverage" (2017) — [Medium](https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f)
- Golus's shader scales alpha by mip level, roughly `alpha *= 1 + lod*0.25` (secondary report) — [lisyarus: Exploring ways to mipmap alpha-tested textures](https://lisyarus.github.io/blog/posts/exploring-ways-to-mipmap-alpha-tested-textures.html)

**Octahedral impostors**
- Ryan Brucks (Epic) published "Octahedral Impostors" in 2018. It captures a mesh from views on an octahedral sphere mapping and blends the closest three frames for smooth transitions. It was used in Fortnite — [shaderbits.com blog](https://shaderbits.com/blog/octahedral-impostors); implementation reference citing it: [meep-engine octahedral impostor README](https://unpkg.com/@woosh/meep-engine@2.124.2/src/engine/graphics/impostors/octahedral/README.md)
- It has been ported widely: Godot (MIT), Three.js, Unity Industry Toolkit "BakeImpostor", Houdini Labs octahedron impostor lens. A 2025 Three.js demo renders 200k trees on a 3072×3072 terrain — [Godot-Octahedral-Impostors](https://github.com/SIsilicon/Godot-Octahedral-Impostors); [Three.js forest demo](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735); [Houdini Labs](https://www.sidefx.com/docs/houdini/nodes/shop/labs--octahedron_impostor_lens.html); [Unity BakeImpostor](https://docs.unity3d.com/Packages/com.unity.industry.toolkit@3.1/manual/Actions/Autogenerated/BakeImpostor.html)
- A 2024 write-up covers impostor baking and rendering, including depth and normals — [arycama: Imposter Baking and Rendering](https://arycama.github.io/2024/10/05/Imposters.html)

**Shadows at distance**
- HZD (unofficial transcript): near shadow cascades use regular shadow maps for vegetation; a distant cascade uses a height-field system — [mirror transcript](https://wiki.kwonsh.duckdns.org/Between-Tech-and-Art--The-Vegetation-of-/Between-Tech-and-Art--The-Vegetation-of-.html)
- Avatar: Frontiers of Pandora (Snowdrop): "even the most distant trees are cleanly shadowed", with no unshadowed 2D billboards on the horizon — [PCGH review](https://www.pcgameshardware.de/Avatar-Frontiers-of-Pandora-Spiel-73347/Specials/Avatar-Frontiers-of-Pandora-Snowdrop-Tech-Benchmarks-Test-1435474/2/)
- A UE 5.1 community plugin adds "foliage shadow imposters" for Nanite foliage, a sign that shadow cost for dense foliage is a known pain point — [UE forum](https://forums.unrealengine.com/t/free-ue5-1-plugin-for-nanite-foliage-add-foliage-shadow-imposters/680365/7)

**Ray tracing for foliage (emerging, NVIDIA-specific)**
- `VK_NV_cluster_acceleration_structure` builds BLAS from pre-generated clusters, referenced rather than copied, for cluster-LOD reuse. `VK_NV_partitioned_acceleration_structure` rebuilds only the changed TLAS partitions. NVIDIA is developing an RTX Mega Geometry *foliage* system on partitioned TLAS. CDPR describes it as intended for future titles (not shipping yet) — [NVIDIA blog: RTX Mega Geometry Vulkan samples](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples); [Vulkan proposal: cluster AS](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_cluster_acceleration_structure.html); [Vulkan proposal: partitioned AS](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_partitioned_acceleration_structure.html); [PC Gamer on Witcher 4 + Mega Geometry](https://www.pcgamer.com/hardware/graphics-cards/nvidias-rtx-mega-geometry-system-will-be-sprucing-up-the-forests-of-the-witcher-4-making-for-some-lush-looking-ray-traced-trees-to-go-witcher-ing-within/)
- Microsoft previewed equivalent DXR clustered geometry (CLAS) and partitioned TLAS (PTLAS) features on 28 Oct 2026 — [DirectX dev blog](https://devblogs.microsoft.com/directx/clas-ptlas-preview/)

### Inferences
- **quat-3d LOD pipeline (medium; this is R9.4 + R6 + R5):** Build a compute `FoliageCullPass` that, per instance, does frustum culling, Hi-Z occlusion (previous-frame depth), picks a LOD by projected size, and computes a cross-fade factor. It appends to per-LOD instance lists and writes `VkDrawIndexedIndirectCommand`s (`vkCmdDrawIndexedIndirectCount`, core in 1.2). This replaces today's per-instance CPU culling. Pad bounds by the maximum wind amplitude.
- **Cross-fade:** With TAA already in the frame, a dithered (screen-door) cross-fade is the natural fit for a deferred GBuffer. Both LODs draw during the transition with complementary hashed or blue-noise masks, and TAA resolves them. Alpha blending is not an option in a GBuffer. Use the Wyman hashed threshold anchored in object space for stability.
- **Alpha-to-coverage applicability:** quat-3d is deferred with TAA, not MSAA, so A2C has little use in the main GBuffer pass. It stays relevant for a forward/transparent pass or an MSAA shadow pass. The higher-value quick wins are (a) alpha-coverage-preserving mip generation in the texture cooker, (b) hashed alpha test, and (c) following the Fortnite lesson by authoring foliage with more opaque triangles and tighter leaf-card silhouettes to cut overdraw. Use early-Z: a depth prepass for masked foliage, then `EQUAL` depth test in the GBuffer pass, so the expensive material shades once.
- **Octahedral impostors (medium):** These fit a deferred engine well. An offline cooker step (or an engine-side bake using the existing renderer) captures albedo, normal and depth atlases on an N×N (hemi-)octahedral grid. Impostor quads write straight into the GBuffer with parallax-corrected depth, so clustered lighting, shadows and TAA "just work". The far LOD can be drawn by the same indirect-draw path. Shadow casting from impostors (rendering impostors into the shadow map) is the cheap distant-shadow answer. HZD's height-field far shadow cascade is an alternative.
- **Cluster/meshlet foliage (long-term):** A Nanite-like path (meshlet DAG, mesh-shader or compute raster, voxel far field) is the 2025–2026 frontier. `VK_EXT_mesh_shader` must stay optional behind a feature check, with a compute-raster or indirect-draw fallback. Treat it as a long-term R-track item after R5/R6. Deferred texturing or a visibility buffer for foliage (HFW) did not carry over even to PC Horizon, so it is low priority.
- **Ray-traced foliage:** Current cluster/PTLAS extensions are NVIDIA-only (`VK_NV_*`). Keep this off the roadmap until a cross-vendor KHR/EXT exists, and keep foliage geometry formats cluster-friendly so the option stays open.

### Gaps
- I could not open Brucks' shaderbits post to verify the frame-count recommendations, the hemi-octahedral variant, or the depth/parallax details. The hemi-octahedral variant (upper hemisphere only, for trees never seen from below) is widely implemented but unverified from the primary source here.
- I found no primary source quantifying dithered-LOD vs. alpha-blend cross-fade cost, or dither/TAA ghosting trade-offs.
- I could not read the McLaren HFW talk, so tile structure and gains are unknown.
- I found no shipped-title numbers for octahedral impostor atlas sizes or memory.

## Q3. Procedural placement and ecosystems: HZD GPU placement, Far Cry 5 biomes, density maps, rule-based biomes, collision with placed objects

### Takeaway
The production reference is HZD's GPU, density-driven, runtime placement. Artists author rules as a graph over 2D world maps (~4 MB/km²). A GPU pipeline evaluates density per layer around the player and emits instances: 500+ asset types, 100k+ objects, ~250 µs average GPU busy time. Far Cry 5 and UE PCG show the offline/editor complement: Houdini biome "recipes" that react to terrain features, and a production-ready, GPU-capable PCG graph as of UE 5.7. quat-3d already synthesises terrain on the GPU, so moving `Scatter` to a compute pass that reads terrain outputs is the natural next step.

### Cited Findings
**Horizon Zero Dawn (Jaap van Muijden, GDC 2017)**
- "GPU-Based Run-Time Procedural Placement in Horizon Zero Dawn" builds the world around the player dynamically. It covers everything from the artist graph editor for placement rules to the GPU algorithms — [GDC Vault 1024120](https://www.gdcvault.com/play/1024120/GPU-Based-Run-Time-Procedural); [official slides PDF](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf); [Guerrilla publication page](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)
- Numbers from the slides: 500+ asset types; 100,000+ objects in scene; ~250 µs average GPU busy load; world data stored as 2D maps at ~4 MB/km²; the GPU pipeline instantiated 64 times, each emitting one layer; the team began with a traditional CPU approach and moved to GPU density-driven real-time placement — [official slides PDF](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)
- HZD grass tilts as the camera pitches down, hiding gaps between grass meshes (GDC 2018 vegetation talk) — [Game Developer](https://gamedeveloper.com/art/video-how-guerrilla-crafted-the-lush-vegetation-of-i-horizon-zero-dawn-i-)

**Far Cry 5 (Étienne Carrier, GDC 2018; Houdini)**
- Procedural tools generate biomes, texture the terrain, set up freshwater networks and generate cliff rocks, to fill about 100 km² of wilderness "with a terrain changing every day" — [GDC Vault 1025557](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far); [SideFX talk page](https://www.sidefx.com/learn/talks/procedural-world-generation-far-cry-5/)
- Biome "recipes react to the physical features of the land to ensure coherence throughout the world" — [PlayStation Blog, Carrier](https://blog.playstation.com/2018/03/22/the-procedural-world-generation-of-far-cry-5/)
- Companion talk, Jeremy Moore, "Terrain Rendering in Far Cry 5": a GPU compute pipeline for LOD, culling, stitching and rendering the height-field terrain — [GDC Vault 1025480](https://gdcvault.com/play/1025480/Terrain-Rendering-in-Far-Cry)

**Ghost of Tsushima grass (Eric Wohllaib, GDC 2021)**
- Covers generating "acres of grass within reasonable memory and performance limits" and rendering and animating individual blades — [GDC Vault 1027214](https://gdcvault.com/play/1027214/contactUs)
- Secondary summaries: individual blades are generated on the GPU, each with its own procedural appearance and animation. Procedural noise clumps blades and drives height, colour and direction per clump. Blades perpendicular to the camera are widened in view space. Distant LOD makes blades twice as wide and half as many. A secondary blog quotes ~83k blades at ~2.5 ms, which I treat as unverified — [GodotGrass reimplementation](https://github.com/2Retr0/GodotGrass); [Unity-Grass (Voronoi clumping)](https://cainrademan.github.io/Unity-Grass/); [Stylized Station thread](https://threadreaderapp.com/thread/1688939411575119872); [hashnode write-up](https://hashnode.com/posts/what-we-can-learn-from-grass-in-ghost-of-tsushima-renders/69e130b2ffbb787634eb9ef4)
- Ghost of Yōtei's GPU-compute renderer roughly doubles the grass and renderable item count vs. Tsushima — [GamingBolt](https://gamingbolt.com/?p=630374)

**Unreal PCG (5.5–5.7)**
- PCG GPU processing suits point processing, runtime generation and static-mesh spawning. It beats CPU only with enough points to fill the GPU, and CPU↔GPU transfer has a cost. Hierarchical Generation or Unbounded grids factor out shared work — [Epic docs: Using PCG with GPU processing](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-with-gpu-processing-in-unreal-engine)
- UE 5.7 declares PCG production-ready and "almost twice as fast as it was in UE 5.5" — [UE 5.7 Preview](https://forums.unrealengine.com/t/unreal-engine-5-7-preview/2658958); [TweakTown](https://www.tweaktown.com/news/107858/unreal-engine-5-7-preview-now-out-with-production-ready-procedural-content-generation-framework/index.html)
- Secondary coverage: most of the Witcher 4 demo's foliage was generated at runtime on the GPU via PCG (available since 5.5, optimised in 5.6) — [80.lv: GPU generation with UE 5.6 PCG](https://80.lv/articles/introduction-to-gpu-generation-with-unreal-engine-5-6-s-pcg)

**Snowdrop (Avatar: Frontiers of Pandora, 2023)**
- The scatter system "might have been built for Avatar ... by now it has become a general Snowdrop feature". Ground foliage uses automated placement with GPU visibility tests — [Massive: Crafting Pandora's landscape](https://www.massive.se/?p=18290); [Massive: Snowdrop's dynamic world](https://www.massive.se/article/an-evolving-pandora-snowdrops-dynamic-world/)

### Inferences
- **quat-3d GPU placement (medium, high value):** Today `Scatter` generates Poisson placements on the CPU. The HZD model maps cleanly onto quat-3d:
  1. Terrain synthesis (R10.2) already produces height, plus potentially slope, curvature, flow/erosion and wetness maps on the GPU.
  2. A `PlacementPass` compute pass per layer (tree species, bush, rock, grass type) evaluates a density expression over those maps plus artist-painted density/biome masks for each tile in a camera-centred ring.
  3. It converts density to points via a deterministic, tile-seeded blue-noise/Poisson pattern thresholded by density (avoid `rand()`; keep tile-seam determinism).
  4. It appends instances to a GPU buffer consumed by the foliage cull/LOD pass (Q2).

  Higher-priority layers (trees) write an occupancy/"footprint" mask that lower layers read to avoid overlap. That is HZD-style layer ordering; 64 pipelines, one layer each.
- **Determinism and CPU needs:** CLAUDE.md's "GPU compute first" asks for a CPU reference where determinism matters. Placement feeds physics (tree trunk colliders in Jolt), nav and AI, so keep the existing CPU `Scatter` as the reference implementation and test the GPU path against it (bitwise or tolerance). Only "important" layers (trees, rocks with colliders) need CPU-side results. Generate them asynchronously per tile, read them back a frame later or run the CPU reference for those tiles, then create Jolt static bodies near the player. Grass and small plants stay GPU-only with no colliders.
- **Rule-based biomes for AI/MCP:** Far Cry 5's "recipes" and HZD's rule graph are data. quat-3d can expose a biome/layer recipe schema (species list, density expression, slope/height/wetness ranges, clustering, exclusion radius) through the existing remote JSON-RPC and MCP bridge. Read it via `json_fields.hpp`, since it is untrusted input. AI agents could then author and tune ecosystems declaratively ("make the north valley a wet birch forest") instead of placing objects one by one. This is a strong differentiator with modest cost.
- **Grass LOD:** Adopt the Tsushima LOD rule (fewer, wider blades with distance; view-space widening of edge-on blades) and the HZD camera-pitch grass tilt as cheap quick wins inside R9.3.

### Gaps
- I could not open the HZD slides past the headline numbers. The exact density→point algorithm (I recall ordered dither or footprint patterns per tile) and the inter-layer collision/footprint mechanism are unverified here and should be read from the PDF.
- The Far Cry 5 vegetation-specific details (asset counts, runtime vs. bake) were not found.
- I found no primary numbers for Tsushima's grass cost; the ~2.5 ms / 83k blades figure is secondary.

## Q4. Shading: leaf translucency/subsurface, two-sided lighting, specular occlusion, foliage AO and self-shadowing

### Takeaway
The standard real-time leaf model is a "two-sided foliage" shading model. Opaque or masked geometry carries a subsurface (transmission) colour. Back-lit light passes through (attenuated by shadowing), the back-face normal is flipped, and the surface is not treated as a thick SSS medium. HZD explicitly covered leaf translucency. In a deferred engine this needs a shading-model ID and a transmission colour in the GBuffer and a transmission term in clustered lighting. AO and specular occlusion for foliage are under-documented in public sources.

### Cited Findings
- UE's Two Sided Foliage shading model lets light pass through the surface for an even, natural look, like sunlight through leaves. Subsurface Color sets the transmitted light and can mask stems and veins. Default Lit renders undersides almost black. The Subsurface model is less accurate for thin surfaces — [Epic docs: Shading models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)
- Practitioner advice (UE 4.7-era forum, verify): keep foliage Masked/Opaque, not Translucent. Subsurface colour is often the base colour, more saturated or darker to imitate multiple bounces. Opacity of ~0.5–0.9 controls how much light passes through in dynamic shadows — [UE forum: Foliage shading in 4.7](https://forums.unrealengine.com/t/foliage-shading-in-4-7/17527/10)
- HZD's vegetation talk covers leaf translucency alongside polygon structure and wind — [Game Developer](https://gamedeveloper.com/art/video-how-guerrilla-crafted-the-lush-vegetation-of-i-horizon-zero-dawn-i-)
- Crysis' GPU Gems 3 ch. 16 includes a shading section for vegetation, balancing quality and efficiency for thousands of on-screen objects — [GPU Gems 3 ch.16](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch16.html)

### Inferences
- **Fit for quat-3d (quick win to medium):**
  - Reserve a shading-model ID in the GBuffer (for example 2–3 bits in the material channel) and add a foliage transmission colour/strength. It can be packed in place of metalness for foliage, since leaves are dielectric.
  - The clustered lighting compute pass branches on the ID. Add a wrapped or back-lit transmission term, `max(0, dot(-N, L))` (or a view-dependent forward-scatter lobe) times transmission colour times shadow. Sample the shadow map with a slight normal offset, or better, use the shadow-map depth difference as thickness, which also gives soft light bleed through canopies.
  - In the GBuffer vertex/fragment shader, flip the normal for back faces (`SV_IsFrontFace` in Slang).
  - Following CLAUDE.md's "Slang interface, not `#ifdef`" rule, this should be a `FoliageMaterial : IMaterial` implementation.
- **AO / self-shadowing (medium):** Common practice (not sourced here) is baked per-vertex canopy AO (darker toward the trunk centre or by depth into the crown), plus a per-instance "crown sphere" normal for bent, smoothed ambient lighting. That should pair with the planned GTAO (R8). Specular occlusion should derive from AO, because leaf-card normal maps produce noisy specular.
- **Shadows:** Alpha-tested shadow casters cost the most. Use simpler shadow LODs (proxy meshes, impostor shadows at range) and per-cascade LOD bias. Do not cast shadows from grass beyond a short range. Use screen-space contact shadows (R8) instead.

### Gaps
- No primary 2018–2026 talk found (in fetchable form) giving a specific leaf BRDF or transmission formula from HZD, Tsushima, Frostbite or Decima.
- No sourced material found on foliage specular occlusion or "crown normal" ambient techniques. The bullets above are practitioner knowledge, not cited.
- No source found on multiple-scattering canopy lighting (e.g. GI or Lumen behaviour for dense foliage beyond forum anecdotes).

## Q5. Interaction: bending/trampling (grass and bush displacement), footprints, cutting

### Takeaway
Shipped interaction is overwhelmingly a camera-centred, top-down GPU texture. Characters, weapons and explosions write displacement or velocity into it, and it relaxes via a spring/damper or decay, so grass and bush shaders read it alongside wind. Tsushima and Yōtei extend this with a "cut buffer" from weapon swings. God of War adds ground-vegetation interaction on top of a 3D wind field. Academic GPU grass (Jahrmann & Wimmer 2017) does per-blade physics with collisions in compute.

### Cited Findings
- Ghost of Yōtei: grass and small plants bend with wind and character movement. Weapon swings feed a "cut buffer" that spawns particles and lets most grass, flowers and small plants be cut — [PlayStation Blog: Ghost of Yōtei tech deep dive](https://blog.playstation.com/?p=411201)
- God of War: ground-vegetation character interaction alongside interactive boneless tree and leaf sway — [GDC Vault 1026036](https://gdcvault.com/play/1026036/Interactive-Wind-and-Vegetation-in)
- Common GPU pattern: a compute shader writes trample direction and amount into a render texture, which then distorts the grass mesh — [Medium: Compute Shaders: Grass Rendering](https://medium.com/dotcrossdot/compute-shaders-grass-rendering-6916a9dd008e)
- Variant: a velocity (RG) plus displacement (BA) texture written by agents and simulated as a spring-mass-damper back to zero, all on GPU — [thuleanx: interactive grass](https://thuleanx.github.io/graphics/inprograss)
- Jahrmann & Wimmer, "Responsive Real-Time Grass Rendering for General 3D Scenes" (I3D 2017): GPU-driven indirect grass with per-blade gravity, wind and collision forces in compute — [paper draft](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf)
- Background: Orthmann et al. 2008, GPU grass collisions via distance maps — [paper](https://www.cg.informatik.uni-siegen.de/sites/www.cg.informatik.uni-siegen.de/files/data/Publications/2008/orthmann08grass.pdf); Habel et al. 2007, instant animated grass — [preprint](https://www.cg.tuwien.ac.at/research/publications/2007/Habel_2007_IAG/Habel_2007_IAG-Preprint.pdf)
- Unreal's Nanite Foliage character interaction was an open community question as of 5.7, so skeletal/Nanite foliage did not trivially support it — [UE forum](https://forums.unrealengine.com/t/ue5-7-nanite-foliage-character-interaction-support-and-future-direction/2698270)

### Inferences
- **quat-3d interaction field (quick win, pairs with R9.3):**
  - Add an `InteractionPass` compute pass that runs before grass generation. Each frame it splats a small SSBO of interactors (capsules from characters, the Jolt `CharacterVirtual`, dynamic rigid bodies near the camera, explosion impulses) into a camera-centred, toroidally scrolled 2D texture (for example 256² over 64 m), storing RG velocity and BA displacement/flattening.
  - It integrates a damped spring so blades recover. Grass compute reads it together with the wind field; bushes and small plants read it in their bending shader.
  - Keep the bound as world-space-snapped texels so the texture doesn't swim.
  - A persistent "flattened/cut" channel (Yōtei's cut buffer) gives trails and footprints in grass. Snow/mud footprints are the terrain-deformation analogue and can share the same scrolling-clipmap infrastructure.
- **Gameplay hookup:** Lua scripting and Jolt contacts can push interactors (explosions, vehicles) into the SSBO, and MCP can do so for testing. All of it is data-parallel GPU work, matching "GPU compute first", with no CPU readback needed.
- **Trees:** Character-tree interaction is rarely more than local wind impulses. Feed explosion and helicopter emitters into the wind field (Q1) instead of per-tree collision. Trunk colliders remain static Jolt bodies from placement (Q3).

### Gaps
- No primary source found detailing Tsushima's or Yōtei's interaction texture resolution, decay model or cut-buffer format.
- No primary AAA source found on bush (as opposed to grass) displacement specifics.

## Q6. Consolidated mapping to quat-3d and recommended priority

### Takeaway
quat-3d already has the right skeleton (render graph with compute passes, TAA, deferred clustered lighting, GPU terrain, instancing, compute grass planned). Foliage quality mostly depends on four shared GPU systems: a wind field, an interaction field, GPU placement, and GPU instance cull/LOD with indirect draws. On top of those come a foliage shading model and impostors. Mesh-shader cluster LOD, voxel far fields and RT foliage are long-term.

### Cited Findings
- Shared wind driving grass, trees, rope and cloth is shipped practice — [PlayStation Blog (Tsushima VFX)](https://blog.playstation.com/?p=345372)
- GPU runtime placement at ~250 µs for 100k+ objects on PS4-era hardware — [HZD slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)
- Opaque high-poly foliage beat masked foliage under Nanite in Fortnite — [Epic Tech Blog](https://www.unrealengine.com/tech-blog/bringing-nanite-to-fortnite-battle-royale-in-chapter-4)
- Voxel far LOD and GPU skeletal wind are the 2025 frontier (experimental in UE 5.7; Witcher 4 demo at 60 fps on PS5) — [Epic docs: Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage); [CDPR press release](https://press.cdprojektred.com/news/1778/cd-projekt-red-and-epic-games-present-the-witcher-4-unreal-engine-5-tech-demo-at-the-state-of-unreal-2025)
- ROADMAP context: R9.3 GPU grass and R9.4 instance culling + LOD are open; R5 GPU-driven and R6 GPU culling are open; R10.2 GPU terrain synthesis and R10.3 GPU particles are open — `ROADMAP.md` (repo)

### Inferences
Priority table. All costs are estimates by the author of these notes, not sourced.

| # | Technique | quat-3d mapping | HW/API | Cost | Priority |
|---|---|---|---|---|---|
| 1 | Global wind field (2D/low-res 3D, gust noise + emitters) | `WindFieldPass` compute; sampled by grass, trees, particles, cloth; double-buffered for motion vectors | Vulkan 1.3 core | S (1–3 days) | **Quick win** |
| 2 | Crysis-style main + detail bending with per-instance phase | Foliage VS in GBuffer + shadow passes; bounds padding; prev-frame wind for TAA velocity | core | S | **Quick win** |
| 3 | Interaction/trample field (velocity + displacement, damped spring; cut channel) | `InteractionPass` compute fed by Jolt characters, Lua or MCP impulses | core | S–M | **Quick win** (with R9.3) |
| 4 | Two-sided foliage shading model (transmission, back-face normal flip) | GBuffer shading-model ID; clustered lighting branch; Slang `FoliageMaterial` | core | S–M | **Quick win** |
| 5 | Alpha coverage–preserving mips + hashed alpha test + masked depth prepass | Texture cooker + Slang alpha-test helper; prepass then `EQUAL` GBuffer | core | S | **Quick win** |
| 6 | Tsushima grass LOD (wider + fewer blades) + view-space widening + HZD camera-pitch tilt | Inside R9.3 compute grass | core | S | **Quick win** (in R9.3) |
| 7 | GPU instance cull + LOD select + dithered cross-fade, indirect draws | `FoliageCullPass` (R9.4/R6/R5), Hi-Z from prev depth, `DrawIndexedIndirectCount` | core 1.2+ | M | **Medium** (next after R9.3) |
| 8 | HZD-style GPU density placement from terrain maps + biome recipes | `PlacementPass` per layer reading R10.2 outputs; CPU `Scatter` kept as reference; collider layers read back async; recipes via JSON-RPC/MCP | core | M–L | **Medium** (high value for AI world-building) |
| 9 | Octahedral / hemi-octahedral impostors writing GBuffer (+ impostor shadows) | Offline bake via engine renderer; far LOD in same indirect path | core | M | **Medium** |
| 10 | Pivot-Painter-style hierarchical wind (2–4 levels) | Cooker bakes per-element pivot/parent/axis into SSBO (BDA) | core | M | **Medium** |
| 11 | Simulated wind (GPU fluid à la God of War, or vorticles à la Tsushima) | Replace noise in #1 with compute sim | core | M–L | **Long-term** |
| 12 | Skeletal/joint wind for near trees (Tsushima, Nanite Dynamic Wind) | Compute bone-spring sim → existing GPU skinning compute or rigid parts | core | L | **Long-term** |
| 13 | Cluster/meshlet foliage + voxel far LOD (Nanite Foliage-like) | Meshlet DAG; mesh shaders optional (`VK_EXT_mesh_shader` guarded) with compute fallback | mesh shader optional | XL | **Long-term** |
| 14 | Deferred texturing / visibility buffer for foliage (HFW) | Would replace GBuffer for masked geo; HFW PC port reverted to GBuffer | core | L | **Low / long-term** |
| 15 | RT foliage via cluster BLAS + partitioned TLAS | `VK_NV_cluster_acceleration_structure` / `VK_NV_partitioned_acceleration_structure` | NVIDIA-only today | XL | **Watch** (no cross-vendor ext) |

- **Cross-cutting correctness items for quat-3d:**
  - Wind and interaction displacement must be identical in GBuffer, shadow and any depth prepass.
  - Motion vectors must include previous-frame deformation, or TAA smears foliage. This is the most common foliage artefact in TAA engines.
  - Cull bounds must be padded for maximum displacement.
  - Dithered LOD transitions should use a temporally stable (object-anchored, hashed) pattern so TAA converges.

### Gaps
- No public cost numbers for any of these techniques on hardware comparable to quat-3d's target GPUs. Costs above are engineering estimates.
- Primary-source verification of several items (HZD dither placement details, Brucks impostor parameters, Tsushima grass cost, HFW deferred texturing internals) was blocked because gdcvault.com, guerrilla-games.com, shaderbits.com, developer.nvidia.com, dev.epicgames.com and 80.lv were not reachable from this session. Allowing those hosts and re-reading the slides would close most gaps.
