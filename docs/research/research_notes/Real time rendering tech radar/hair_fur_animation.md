# Real-time Hair, Fur, Cloth and Character Animation / Simulation (2018–2026) — fit to quat-3d

Research notes, collected 2026-11-27. Method: web search (snippets and primary-source landing pages) and one full
fetch (Jolt `ReleaseNotes.md`). **Several primary hosts could not be fetched from this research container**
(`www.ea.com`, `jrouwe.github.io`, `www.advances.realtimerendering.com`), so details from the Frostbite blog,
Jolt HTML docs and the SIGGRAPH Advances 2019/2025 PDFs come from search-result snippets of those pages, not from
reading them in full. Items marked "(background, unverified here)" are well-known prior knowledge that I could not
confirm with a source in this session; the report writer should treat them as such.

---

## 1. Strand-based hair rendering (Frostbite/EA SEED, TressFX, HairWorks, Unreal Groom, id Tech; shading, shadowing, OIT, LOD)

### Takeaway
Shipping strand hair has converged on a GPU-compute-heavy pipeline: strands as polylines → (software or hardware)
rasterised visibility with heavy anti-aliasing (MSAA visibility buffer or a compute "one lane per segment"
rasteriser) → strand-space or deferred shading with a Marschner-derived BSDF plus an approximate multiple-scattering
term → deep-opacity / voxel transmittance for self-shadowing → LOD to fewer curves, then cards/meshes at distance.
It has now shipped on consoles at 60 FPS (Dragon Age: The Veilguard, 2024; Indiana Jones and the Great Circle,
SIGGRAPH 2025 talk), but is memory- and compute-intensive and is usually a toggleable feature.

### Cited Findings
**Frostbite / EA (canonical industry reference)**
- Sebastian Tafuri, "Strand-based Hair Rendering in Frostbite", SIGGRAPH 2019 Advances in Real-Time Rendering in Games; states strand-based rendering (fibres as individual curves) is the offline state of the art, and the single-scattering BSDF is based on Marschner et al. 2003 — [SIGGRAPH 2019 Advances slides](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf)
- Frostbite blog series (two parts) frames the goal as "a step change in real-time hair" approaching offline results; rendering topics: shading model, multiple scattering, anti-aliased hair; physics topics: a Eulerian/Lagrangian model, constraints and a style-preservation technique — [Frostbite Hair Rendering and Simulation (Part 1)](https://www.ea.com/news/frostbite-hair-rendering-and-simulation); [Part 2](https://www.ea.com/frostbite/news/frostbite-hair-rendering-and-simulation-2)
- Blog contrasts with hair cards: cards are "relatively cheap computationally" but reaching high quality in rendering or simulation is hard; strands are tessellated into points, with "most strands featuring 25 points per strand" in one long-hair asset — [Frostbite blog Part 1 (careers mirror)](https://careers.ea.com/frostbite/news/frostbite-hair-rendering-and-simulation)
- Shipped in Dragon Age: The Veilguard (BioWare, Frostbite, 2024): ~50,000 individual strands per character; 60 FPS on PS5, Xbox Series X and PC; >4 years of BioWare/Frostbite collaboration — [EA: Strand Hair in Dragon Age: The Veilguard](https://www.ea.com/news/strand-hair-dragon-age-the-veilguard); [TechRadar](https://techradar.com/gaming/consoles-pc/dragon-age-the-veilguards-hair-technology-features-50-000-individual-strands-per-character); [Microsoft Developer article](https://developer.microsoft.com/en-us/games/articles/2024/12/bioware-dragon-age-the-veilguard-strand-hair-technology/)
- Lead engineer describes it as memory- and computation-intensive; each hair asset has a high memory footprint, so players can turn it off for performance — [Microsoft Developer](https://developer.microsoft.com/en-us/games/articles/2024/12/bioware-dragon-age-the-veilguard-strand-hair-technology/); [TechRadar](https://techradar.com/gaming/consoles-pc/dragon-age-the-veilguards-hair-technology-features-50-000-individual-strands-per-character)

**id Tech / MachineGames — Indiana Jones and the Great Circle**
- SIGGRAPH 2025 Advances talk (Kulikov) "Strand Hair in IJGC": pipeline uses software rasterisation with **one lane per segment** writing into a **visibility atlas**, followed by **strand-space shading** — [SIGGRAPH 2025 Advances slides](https://www.advances.realtimerendering.com/s2025/content/Strand%20Hair%20in%20IJGC%20-%20Final%20Slides%20(Post-Conference).pdf) (snippet only; PDF host blocked from this container)

**Unreal Engine Groom**
- Epic's 2020 tech blog (UE 4.24 era): strands converted to polylines drawn as thin camera-facing triangle strips; light transmission via **Deep Opacity Maps + runtime voxelisation**; multiple scattering approximated with **dual scattering** — [Unreal tech blog: An early look at next-generation real-time hair and fur (Jan 2020)](https://www.unrealengine.com/en-US/tech-blog/an-early-look-at-next-generation-real-time-hair-and-fur)
- UE 4.26 visibility options: MSAA visibility buffer (default), per-pixel linked list (PPLL; higher quality/costlier, aimed at film), and a compute-based rasteriser (lighter); shadowing via dedicated deep shadow maps and voxel tracing — [UE4 character art deep-dive slides (Kobayashi, Epic Japan)](https://www.slideshare.net/slideshow/ue4characterartdiveonline01hiroyukikobayashi-249860768/249860768) (secondary; summarised from search snippet)
- Groom LOD: each LOD can be strands, cards or meshes; "Auto" LOD adapts curve count to screen size; GPU LOD mode is finer-grained but strands-only, CPU mode coarser but supports all three representations; `r.HairStrands.UseCardsInsteadOfStrands` forces cards — [UE docs: LOD for grooms](https://dev.epicgames.com/documentation/unreal-engine/setting-up-level-of-detail-for-grooms-in-unreal-engine?lang=en-US); [UE docs: cards and meshes for grooms](https://dev.epicgames.com/documentation/unreal-engine/setting-up-cards-and-meshes-for-grooms-in-unreal-engine)
- Hair Card Generator (experimental) auto-generates cards from strand grooms, also as a MetaHuman Dataflow tool — [UE docs: Hair Card Generator](https://dev.epicgames.com/documentation/unreal-engine/hair-card-generator-for-grooms-in-unreal-engine); [MetaHuman Dataflow card generator](https://dev.epicgames.com/documentation/metahuman/hair-card-generator-in-dataflow-in-unreal-engine)
- Groom physics is driven by Niagara — [UE docs: groom components and assets](https://dev.epicgames.com/documentation/unreal-engine/groom-components-and-assets-in-unreal-engine); scalability guidance — [UE docs: groom scalability and performance](https://dev.epicgames.com/documentation/ja-jp/unreal-engine/groom-scalability-and-performance-with-unreal-engine)

**AMD TressFX**
- GPU hair/fur rendering + simulation library — [GPUOpen TressFX](https://gpuopen.com/tressfx/)
- TressFX 5.0 (GDC 2022) shipped as an open-source integration for UE 4.26/4.27/5, later updated for UE 5.4 as an engine patch (`git am`); strand and collision-mesh vertices support up to 16 bone bindings (was 4); new simulation editor and SDF editor — [GPUOpen: TressFX 5.0](https://gpuopen.com/learn/tressfx5/); [GPUOpen: TressFX 5.0 for UE 5.4](https://gpuopen.com/learn/tressfx-5-0-unreal-engine-5-update/); [TressFX 5.0 Developer Guide (PDF)](https://gpuopen.com/download/TressFX5.0_DeveloperGuide.pdf)
- No standalone (non-Unreal) TressFX 5 library was found; the TressFX 4.x standalone SDK is the last engine-agnostic release (search found only the UE-patch form of 5.0) — [GPUOpen TressFX](https://gpuopen.com/tressfx/)
- TressFX OIT: per-pixel linked list (head-pointer UAV + node UAV, sort only the front k fragments, blend the rest unsorted); memory scales with resolution and can overflow close-up — [AMD GDC14 "Grass, fur and all things hairy"](https://fr.slideshare.net/DevCentralAMD/grass-fur-and-all-things-hairy-amd-at-gdc14); [TressFX 2.0 and Beyond](https://www.slideshare.net/slideshow/gs4147-billbilodeau/28502778)
- TressFX 3.1 "ShortCut" OIT: bounded memory, uses `InterlockedMin` to keep the k nearest fragments plus accumulated alpha; inspired by Eidos-Montréal and Hybrid Transparency; faster but slightly lower quality than PPLL — [GPUOpen: TressFX 3.1](https://gpuopen.com/tressfx-3-1/); Godot notes ShortCut needs 4 passes — [V-Sekai/Godot OIT decision](https://v-sekai.github.io/manuals/decisions/20210811-godot-transparency.html)
- TressFX self-shadowing uses a simplified deep shadow map, separate from hair→scene shadow casting; self-shadowing is "essential to the volumetric look" — [AMD GDC14 slides](https://fr.slideshare.net/DevCentralAMD/grass-fur-and-all-things-hairy-amd-at-gdc14)
- Background on adaptive OIT used by many hair renderers: Salvi et al., "Adaptive Transparency", HPG 2011 — [Intel PDF](https://www.intel.com/content/dam/develop/external/us/en/documents/37944-adaptive-transparency-hpg11.pdf)

**NVIDIA HairWorks (why it fell out of favour)**
- The Witcher 3 (2015) HairWorks used tessellation-generated strands at a 64x tessellation factor per AMD's analysis; it crippled performance, especially on AMD GPUs; players capped tessellation to 8x/16x in driver; CD Projekt allowed disabling it; this turned into a public AMD-vs-NVIDIA dispute — [PCWorld](https://www.pcworld.com/article/427598/amd-radeon-graphics-run-witcher-3-just-fine-unless-nvidia-hairworks-is-enabled-and-thats-ok.html); [TechSpot benchmark "HairWorks: Crippling Performance"](https://www.techspot.com/review/1006-the-witcher-3-benchmarks/page6.html); [PC Perspective](https://pcper.com/2015/05/nvidia-under-attack-again-for-gameworks-in-the-witcher-3-wild-hunt/)

**Research (2025–2026)**
- Lipp, Jarabo, Wimmer, Bode, I3D 2026 (PACMCGIT, DOI 10.1145/3820015): deferred software rasterisation of strands generated implicitly from hair meshes, aimed at far-field hair, efficient "even at a single sample per pixel" via deferred shading + reconstruction; claims cost comparable to 4x-MSAA quality — [TU Wien project page](https://www.cg.tuwien.ac.at/research/publications/2026/LIPP-2026-HAIR/); [arXiv 2607.04230](https://arxiv.org/abs/2607.04230)
- EA patent "Systems and methods for hair rasterization": GPU hair rasteriser culls segments with conservative depth and bins visible segments into per-tile buckets — [USPTO 11551387](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11551387)

**Shading models**
- Marschner et al. 2003 (R, TT, TRT lobes) is the base of Frostbite's single-scattering BSDF — [Frostbite SIGGRAPH 2019 slides](https://www.advances.realtimerendering.com/s2019/hair_presentation_final.pdf)
- Chiang, Bitterli, Tappan, Burley, "A Practical and Controllable Hair and Fur Model for Production Path Tracing", CGF 2016: energy-conserving, near-field, logistic azimuthal roughness, artist-friendly parameterisation (multiple-scattering albedo → absorption) — [Disney Animation](https://disneyanimation.com/publications/a-practical-and-controllable-hair-and-fur-model-for-production-path-tracing); [EG Digital Library](https://diglib.eg.org/handle/10.1111/cgf12830)
- Artist-friendly earlier model: Sadeghi et al. "An Artist Friendly Hair Shading System" (Disney) — [PDF](https://la.disneyresearch.com/wp-content/uploads/An-Artist-Friendly-Hair-Shading-System-Paper.pdf)

### Inferences
- **Architecture for quat-3d (long-term, R-track)**: strand hair maps cleanly onto the "GPU compute first" render graph: (1) compute: sim/skin strands (roots attached to the skinned scalp produced by the existing skinning pass); (2) compute: LOD/cull + strand → segment expansion; (3) compute software rasteriser writing a visibility buffer/atlas (id Tech, Frostbite patent, Lipp 2026 all point this way — avoids the sub-pixel-triangle inefficiency that sank HairWorks-style tessellation); (4) compute shading with Marschner/Chiang-derived BSDF + dual-scattering-style multiple scattering; (5) composite into the HDR buffer after the clustered lighting pass, before TAA. Hair cannot live in the deferred GBuffer (needs per-fibre coverage/opacity), so it is a separate path like TransparentPass.
- A software rasteriser needs 64-bit or packed 32-bit atomics (`shaderBufferInt64Atomics` / `VK_KHR_shader_atomic_int64`) for depth+ID visibility writes; Slang supports these. Must be feature-checked for MoltenVK/KosmicKrisp (P-track).
- TAA interacts badly with sub-pixel strands; production renderers rely on MSAA visibility or many samples per pixel before TAA. Plan for a hair-specific velocity/reactive mask.
- **Cheap path first**: hair cards (alpha-to-coverage or alpha-tested + dithered + TAA) on the existing mesh/GBuffer pipeline, with an anisotropic Marschner-approximation shading model in the GBuffer shading-model ID, is the industry-standard fallback and the LOD tail of every strand system (UE LODs to cards). Recommended **medium** priority; strand hair **long-term**.
- HairWorks' lesson: avoid hardware tessellation / geometry-amplification of strands into tiny triangles; compute rasterisation is the modern answer.

### Gaps
- Could not read the Frostbite blog Part 2, SIGGRAPH 2019 or 2025 PDFs in full (hosts blocked), so exact ms timings, strand counts per frame, and the multiple-scattering/AA specifics are missing.
- Brian Karis's "Physically Based Hair Shading in Unreal" (SIGGRAPH 2016 PBR course) and UE's current shading model (Marschner-based with Chiang-style terms) — (background, unverified here).
- Deep Opacity Maps (Yuksel & Keyser 2008) original source not retrieved; only Epic's mention of DOM + voxelisation.
- No reliable source on whether NVIDIA formally deprecated HairWorks; only the 2015 controversy is documented.
- TressFX 4.1 standalone SDK details (GitHub GPUOpen-Effects/TressFX) not fetched.

---

## 2. Hair simulation (PBD/XPBD/Cosserat strands, Frostbite physics, wind; costs)

### Takeaway
Real-time strand simulation is GPU, position-based (PBD/XPBD or Cosserat rods), sub-stepped, with long-range
attachments to stop stretch, local shape/style preservation, and a grid (Eulerian) pass for hair–hair interaction
and volume. Notably, **Jolt Physics 5.6 now ships a GPU Cosserat-rod hair simulator with a Vulkan compute backend**,
which is a direct fit for quat-3d since it already uses Jolt.

### Cited Findings
- Frostbite's hair physics uses a combined Eulerian/Lagrangian model, several constraint types and a style-preservation technique — [Frostbite blog Part 1](https://www.ea.com/news/frostbite-hair-rendering-and-simulation)
- **Jolt 5.6.0**: "strand-based hair simulation running on the GPU, based on Cosserat rods"; compute shader backends for DX12, Vulkan and Metal; hair–hair interaction by "accumulating the average velocity in a grid"; environment collision "only supports ConvexHull and CompoundShapes"; roots skinned to the scalp mesh; long-range attachments limit stretch — [Jolt ReleaseNotes.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md); release coverage calls hair "the star of the show" — [GameFromScratch: Jolt 5.6](https://gamefromscratch.com/jolt-physics-5-6-released/); [gamedev.net: Jolt 5.6](https://gamedev.net/news/4436-jolt-physics-56-released/)
- Jolt 5.7.0: fixed hair jitter on macOS/iOS; added a non-cached host-memory fallback in `ComputeSystemVK` for GPUs such as Mali — [Jolt ReleaseNotes.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)
- Jolt 5.4.0 added Cosserat rods ("stick constraint with orientation", for attaching geometry such as vegetation) to the soft-body system — [Jolt ReleaseNotes.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)
- TressFX 5 simulation: GPU, strands bound to up to 16 bones, SDF collision via an SDF editor — [TressFX 5.0 Developer Guide](https://gpuopen.com/download/TressFX5.0_DeveloperGuide.pdf); [GPUOpen TressFX 5](https://gpuopen.com/learn/tressfx5/)
- Unreal groom simulation runs through Niagara (GPU) — [UE docs: groom components](https://dev.epicgames.com/documentation/unreal-engine/groom-components-and-assets-in-unreal-engine)
- XPBD foundation: Macklin, Müller, Chentanez 2016, "XPBD: Position-Based Simulation of Compliant Constrained Dynamics" — compliance makes stiffness independent of iteration count/timestep — [XPBD PDF](https://matthias-research.github.io/pages/publications/XPBD.pdf)
- "Small Steps in Physics Simulation" (Macklin et al., SCA 2019): many sub-steps with one iteration each beats many iterations — [EG Digital Library](https://diglib.eg.org/handle/10.1145/3309486-3340247); summarised in Müller's slides reproduced in [Cornell CS5643 PBD slides](https://www.cs.cornell.edu/courses/cs5643/2025sp/slides/13pbd.pdf)

### Inferences
- **Quick-to-medium win**: evaluate Jolt's hair module (it is already a dependency; it uses its own `ComputeSystemVK`). Integration questions: (a) can Jolt's compute system share quat-3d's `VkDevice`/queues and VMA, or does it allocate its own memory (CLAUDE.md requires VMA for all allocations); (b) output strand positions must be consumed by quat-3d's render graph without CPU round-trip; (c) collision limited to convex hulls/compounds — body proxies would be capsules/convex hulls from the ragdoll/physics skeleton. If it cannot share the device, a native Slang XPBD/Cosserat strand solver following the same design is the fallback (long-term).
- Simulate only "guide" strands (hundreds to low thousands) and interpolate render strands on GPU — standard practice in TressFX/Unreal/Frostbite (background, unverified here for exact numbers).
- Wind: apply as an external force per strand vertex, sampled from a global wind field shared with vegetation (R-track), cheap once the field exists.

### Gaps
- No primary-source per-frame ms costs for any strand sim were retrievable (Frostbite/Jolt HTML docs blocked).
- Whether Jolt's `ComputeSystemVK` can accept an externally created device/allocator is not documented in the release notes.

---

## 3. Fur (shell + fin, modern shell texturing, fur LOD)

### Takeaway
Shell (+ fin) fur remains the dominant cheap fur technique for short fur on characters/animals and is still used in
recent AAA engines (Capcom's RE Engine presented a modern "Shell Fur" at RE:2023). Strand systems (TressFX, UE Groom)
handle fur at higher cost. Claims that specific titles (Ghost of Tsushima, RDR2) use shells could not be verified.

### Cited Findings
- Shell texturing: stacking multiple mesh layers (shells) with noise-based alpha masks to fake volumetric fur/grass/moss — [80.lv](https://80.lv/articles/classic-video-games-trick-for-rendering-grass-fur/); common in older games because it's far cheaper than strands — [Xenoblade rendering research: fur](https://scanmountgoat.github.io/xenoblade-rendering-research/fur.html)
- Capcom R&D, RE:2023 "New Rendering Features Rundown (HDR Grading, Shell Fur, Signed Distance Field)" — RE Engine shell fur — [Docswell CAPCOM_RandD](https://www.docswell.com/s/CAPCOM_RandD/KENVMJ-RE2023)
- Shadow of the Colossus used a layered "shell" approach for the colossi's fur (cited by another studio as inspiration) — [Creative Bloq](https://www.creativebloq.com/3d/how-the-guild-was-2-art-team-looked-to-retro-game-shadow-of-colossus-to-solve-a-modern-problem)
- RDR2 and Ghost of Tsushima fur methods: only forum speculation found (no developer statement) — [Polycount thread](https://polycount.com/discussion/comment/2749520/)
- TressFX targets both hair and fur (GPU strands) — [GPUOpen TressFX](https://gpuopen.com/tressfx/); Unreal grooms cover hair and fur — [Unreal tech blog 2020](https://www.unrealengine.com/en-US/tech-blog/an-early-look-at-next-generation-real-time-hair-and-fur)

### Inferences
- **Quick win for quat-3d**: shell fur fits the existing pipeline: instanced draws (R9.1 instancing) of N shells extruding along the skinned normal from the compute-skinning output, alpha-tested (writes the GBuffer, so no OIT needed) with TAA-friendly dithering; fins via a compute pass emitting silhouette-edge quads. Shell count drives LOD (e.g. reduce shells with screen size). Can reuse a GBuffer "fur/hair" shading-model ID shared with hair cards.
- A modern refinement is to render shells via a mesh shader (VK_EXT_mesh_shader, optional) or a single compute-expanded vertex buffer to cut draw overhead (background, unverified here).

### Gaps
- Capcom's slides not read in detail (only title found); no fetched details on their shell count/LOD.
- No verified 2018–2026 shipped-title list for shell fur beyond Capcom RE Engine.

---

## 4. Cloth (XPBD, Jolt soft bodies, NvCloth, GPU cloth)

### Takeaway
XPBD with small sub-steps is the de-facto real-time cloth formulation. Jolt (already in quat-3d) has a mature,
multithreaded CPU soft-body solver with compliance-based edges, dihedral bends, LRA tethers, volume constraints and
**skinning constraints** designed for character cloth — making it the obvious first step. NvCloth is a legacy
GameWorks library (PhysX 3.x solver lineage).

### Cited Findings
- Jolt soft bodies simulate cloth and soft balls, with contact listeners, bend constraints (distance and dihedral) and skin constraints for skinned meshes; edges have rest length and compliance (inverse stiffness); inverse-mass 0 pins vertices; soft bodies collide with rigid bodies but not with each other — [Jolt-JNI soft-body intro](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/softbody.html); [Jolt docs (search index)](https://docsearch.algolia.com/mcp/docs/repo/jrouwe/joltphysics)
- Jolt version history: 5.0.0 skinning constraints, dihedral bend, long-range attachment (tethers), vertex radius, `SoftBodyContactListener`, `CreateConstraints` from faces; 5.1.0 constraint grouping enabling multithreading of LRA/bend/volume/skinned constraints, ~10% faster dihedral bends; 5.3.0 fixed a determinism race; 5.4.0 Cosserat rods, double-sided faces; 5.5.0 triangle thickness and scaled-shape collision fixes — [Jolt ReleaseNotes.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)
- Godot (Jolt-backed since 4.6) documents soft-body support as more robust under Jolt than GodotPhysics — [Godot soft body docs](https://docs.godotengine.org/en/latest/tutorials/physics/soft_body.html)
- Render meshes often need re-meshing (duplicate vertices, insufficient subdivision) before cloth sim — [Jolt-JNI soft-body intro](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/softbody.html)
- XPBD (Macklin/Müller/Chentanez 2016) — [PDF](https://matthias-research.github.io/pages/publications/XPBD.pdf); Small Steps (SCA 2019) — [EG DL](https://diglib.eg.org/handle/10.1145/3309486-3340247); Müller's Ten Minute Physics tutorials (e.g. rigid bodies, joints) — [22-rigidBodies](https://matthias-research.github.io/pages/tenMinutePhysics/22-rigidBodies.pdf), [25-joints](https://matthias-research.github.io/pages/tenMinutePhysics/25-joints.pdf); detailed PBD body paper (SCA 2020) — [PBDBodies.pdf](https://matthias-research.github.io/pages/publications/PBDBodies.pdf)
- Newer alternative solver: Vertex Block Descent (Chen et al., 2024) — [arXiv 2403.06321](https://arxiv.org/html/2403.06321v3)
- NvCloth: "low level access to a cloth solver designed for realtime interactive applications", collision suited to animated characters; solver backend of PhysX 3.3/3.4 cloth; v1.1.6; used by O3DE — [NVIDIA NvCloth docs](https://docs.nvidia.com/gameworks/content/gameworkslibrary/physx/nvCloth/index.html); [O3DE NVIDIA Cloth](https://docs.o3de.org/docs/user-guide/interactivity/physics/nvidia-cloth/)

### Inferences
- **Medium priority, high fit**: character cloth via Jolt soft bodies using skinning constraints (back-stop/max-distance to the animated pose), driven from quat-3d's CPU animation pose each frame; output vertices uploaded to a dynamic vertex buffer that bypasses (or post-applies) GPU skinning for the cloth submesh. Keep vertex counts modest (CPU solver). Requires a cooking step that welds/remeshes glTF render meshes and authors pin/skin weights (vertex colour channel convention).
- **GPU cloth (long-term)**: a Slang XPBD (graph-colouring or Jacobi) cloth compute pass would satisfy "GPU compute first" for many simultaneous cloth instances, but Jolt CPU is the deterministic reference CLAUDE.md asks for.
- NvCloth is not recommended: legacy GameWorks, CUDA/DX11 GPU paths, no Vulkan path known (background, unverified here).

### Gaps
- Could not confirm whether Jolt's soft-body solver is formally XPBD (compliance model matches XPBD, but no source states it).
- No current info found on NvCloth archive status, Unreal Chaos Cloth or ML-deformer cloth.

---

## 5. Motion matching and learned motion matching

### Takeaway
Motion matching (Ubisoft, For Honor, GDC 2016) — per-frame nearest-neighbour search of a feature database (future
trajectory + current pose features) over mocap — is now mainstream, including in Unreal (Pose Search plugin + Game
Animation Sample). Learned Motion Matching (SIGGRAPH 2020) replaces the database with small networks to cut memory.

### Cited Findings
- Simon Clavet, "Motion Matching and The Road to Next-Gen Animation", GDC 2016 (Ubisoft Montreal); For Honor first shipped game; brute-force search of mocap each frame, needs large data and CPU — [GDC Vault](https://www.gdcvault.com/play/1022985/Motion-Matching-and-The-Road); [Game Watch (JP)](https://game.watch.impress.co.jp/docs/news/749136.html); [GamingBolt](https://gamingbolt.com/ubisoft-introduces-the-next-generation-of-animation-at-gdc). Note: one forum post credits a different presenter; GDC Vault lists Clavet.
- Later adoption claims (UFC 3, The Last of Us Part II) come from forum posts only — [ResetEra](https://www.resetera.com/threads/motion-matching-the-cutting-edge-technique-powering-the-last-of-us-2.49388/) (low reliability)
- Holden, Kanoun, Perepichka, Popa, "Learned Motion Matching", ACM TOG / SIGGRAPH 2020: decomposes MM into steps (decompressor, stepper, projector) and replaces each with a learned network; stores no animation data, memory stays small as data grows — [Ubisoft La Forge](https://www.ubisoft.com/en-us/studio/laforge/news/3NWqwcMU9lMfumgfPAEfka/learned-motion-matching); [paper PDF](https://www.theorangeduck.com/media/uploads/other_stuff/Learned_Motion_Matching.pdf); [author page with code](https://www.theorangeduck.com/page/learned-motion-matching)
- Unreal: motion matching lives in the Pose Search plugin and can replace state machines/blend spaces; debugger tooling exists; Game Animation Sample updated for UE 5.7 (and reportedly 5.8) — [UE docs: Motion Matching](https://dev.epicgames.com/documentation/unreal-engine/motion-matching-in-unreal-engine?lang=en-US); [UE docs: MM debugging](https://dev.epicgames.com/documentation/unreal-engine/motion-matching-debugging-in-unreal-engine); [Unreal tech blog: GASP UE 5.7](https://www.unrealengine.com/en-US/tech-blog/explore-the-updates-to-the-game-animation-sample-project-in-ue-5-7)
- Physics-based follow-up from La Forge: DReCon (data-driven responsive control of physics-based characters, motion matching driving a physics character) — [Ubisoft La Forge DReCon](https://www.ubisoft.com/en-us/studio/laforge/news/VjEIwquaIyEZZSw5RZI0V/drecon-datadriven-responsive-control-of-physicsbased-characters)

### Inferences
- **Medium priority for quat-3d (G-track)**: implement classic MM on CPU first (feature DB built offline from glTF clips: root trajectory points at +10/20/30 frames, foot positions/velocities, hip velocity; normalised; KD-tree or AABB-accelerated brute force), sitting alongside the existing blend tree/state machine as a node. It depends on inertialization (§6) for transitions and on good mocap data; Holden's open-source reference code (linked from the author page) is a compact C++ template.
- Search cost is per character per N frames; for crowds of NPCs (Recast/behaviour trees) the search could move to a compute pass, but CPU is fine for a handful of characters. LMM is long-term/optional (needs a training pipeline).
- An MCP/remote hook to inspect the matched clip/frame and cost breakdown would be a cheap AI-tooling win (A-track).

### Gaps
- Unreal Pose Search "production-ready" status for 5.7/5.8 not confirmed.
- No primary costs (ms/character) found for MM implementations.

---

## 6. Inertialization

### Takeaway
Inertialization (Bollo, Gears of War 4) replaces cross-fades with a post-process that decays the source→target pose
offset over time, so only one pose is evaluated during transitions — cheaper and higher quality. It is the standard
transition method in UE and the natural companion to motion matching.

### Cited Findings
- David Bollo, "Inertialization: High-Performance Animation Transitions in Gears of War", GDC 2018; replaces blended transitions with a post-process; covers vector and quaternion maths and engine integration; removes the need to evaluate multiple states during transitions — [GDC Vault](https://gdcvault.com/play/1025165/Inertialization-High-Performance-Animation-Transitions); [Game Developer](https://gamedeveloper.com/art/video-high-performance-animation-transitions-in-i-gears-of-war-i-)
- Earlier SIGGRAPH 2017 talk "High Performance Animation in Gears of War 4" — [SIGGRAPH history PDF](https://history.siggraph.org/wp-content/uploads/2022/09/2017-Talks-Bollo_High-Performance-Animation-in-Gears-of-War-4.pdf)
- Unreal defines inertialization as making the delta between two blend sources go to zero over time; exposed as blend node / `IInertializerBlend` in UAF — [UE docs: blend nodes](https://dev.epicgames.com/documentation/unreal-engine/animation-blueprint-blend-nodes-in-unreal-engine); [UAF IInertializerBlend API](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/UAFAnimGraph/IInertializerBlend)
- 2026 follow-up: "Half Pound Filter for Real-Time Animation Blending" treats inertialization as the high-performance cross-fade alternative — [arXiv 2602.21702](https://arxiv.org/html/2602.21702)

### Inferences
- **Quick win**: inertialization is a small, self-contained addition to quat-3d's animator (per-joint offset + velocity captured at transition, decayed with Bollo's quintic or Holden's critically-damped spring); it can replace crossfade in `AnimStateMachine` and makes transitions cost one pose evaluation. Testable headlessly (pure math, deterministic).

### Gaps
- GDC Vault content (exact polynomial formulation) not fetched; Holden's "dead blending"/spring-based inertialization articles not retrieved.

---

## 7. IK: FABRIK, CCD, full-body IK, foot IK / terrain adaptation

### Takeaway
Analytic two-bone IK (legs/arms) + raycast foot placement with pelvis offset is the baseline; FABRIK/CCD handle
chains; modern full-body solvers (Unreal Control Rig FBIK) are position-based (PBD-style) rather than classic FABRIK.

### Cited Findings
- Unreal's Control Rig Full Body IK (FullBodyIK plugin) is a procedural tool for ground alignment and reaching, rooted at hips/pelvis, built on a **Position Based IK** framework — [UE docs: Control Rig Full Body IK](https://dev.epicgames.com/documentation/unreal-engine/control-rig-full-body-ik-in-unreal-engine)
- Community foot-placement with FABRIK + capsule traces is a common UE pattern; reported knee-flipping without pole targets; FBIK solver changed in UE 5.4 and users report feet not fully reaching goals — [UE forum: FABRIK foot placement](https://forums.unrealengine.com/t/ik-fabrik-blueprint-tutorial-for-proper-foot-placement-with-capsule-collision/11927/22); [UE forum: FBIK not reaching targets](https://forums.unrealengine.com/t/control-rigs-full-body-ik-not-fully-reaching-its-targets/2531156)

### Inferences
- **Quick win**: two-bone analytic IK + foot-placement (raycast via Jolt, pelvis drop, foot-normal alignment, IK weight from animation curves) after the blend tree and before `compute_world_transforms()`. FABRIK (Aristidou & Lasenby 2011, background, unverified here) for spines/tails/tentacles and look-at chains; CLAUDE.md already lists an optional FABRIK solver under `animation/ik/`. A PBD-style full-body IK can reuse XPBD infrastructure later (long-term).
- Use `CharacterVirtual` ground contacts for slope/step info to drive foot IK cheaply.

### Gaps
- The FABRIK paper (Aristidou & Lasenby, Graphical Models 2011) and CCD references were not retrieved.

---

## 8. Procedural / physics-based animation (active ragdolls, powered ragdolls, Euphoria-style)

### Takeaway
Jolt already provides the building blocks: ragdolls from skeletons, a `SkeletonMapper` between animation and ragdoll
skeletons, and `Ragdoll::DriveToPoseUsingMotors` (now with position+velocity drive) for powered/active ragdolls and
hit reactions. Learned controllers (DReCon) are the research frontier.

### Cited Findings
- Jolt: skeleton mapper converts between animated and ragdoll skeletons (v2.0.0); `Ragdoll::DriveToPoseUsingMotors` (hinge support 5.4.0; position+velocity variant 5.6.0); `RagdollSettings::CalculateConstraintPriorities` boosts root-ward joint priority (5.5.0); `Ragdoll::ResetWarmStart` (5.0.0) — [Jolt ReleaseNotes.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)
- Jolt samples cover keyframed/kinematic ragdolls, motor-driven ragdolls and skeleton mapping — [GameFromScratch overview](https://gamefromscratch.com/the-jolt-physics-engine/); [Jolt docs index (search)](https://docsearch.algolia.com/mcp/docs/repo/jrouwe/joltphysics)
- DReCon (Ubisoft La Forge, SIGGRAPH Asia 2019): motion-matching kinematic controller + RL-trained policy driving a physically simulated character — [Ubisoft La Forge](https://montreal.ubisoft.com/en/drecon-data-driven-responsive-control-of-physics-based-characters/)
- Unreal's Game Animation Sample references physics-driven animation workflows — [gamedev.net summary](https://gamedev.net/news/5051/) (secondary)

### Inferences
- **Medium priority**: death/knockdown ragdolls (kinematic → dynamic switch) and partial powered ragdoll hit reactions using `SkeletonMapper` + `DriveToPoseUsingMotors`, blended back with inertialization. Euphoria-style balance controllers / DReCon are long-term research.

### Gaps
- No primary 2018–2026 source on Euphoria/NaturalMotion techniques was retrieved.

---

## 9. Animation compression (ACL)

### Takeaway
ACL (Nicholas Frechette, MIT) is the industry-standard open-source animation codec and, since UE 5.3, Unreal's
default codec. It is a header-only-style C++ library that would slot into quat-3d's offline cooking pipeline.

### Cited Findings
- ACL is MIT-licensed; UE plugin integrates ACL v2.1.0 for UE4/UE5 — [acl-ue4-plugin GitHub](https://github.com/nfrechette/acl-ue4-plugin); [ACL README](https://raw.githubusercontent.com/nfrechette/acl/develop/README.md)
- Since UE 5.3, ACL ships with the engine as the default animation compression codec — [Frechette: ACL in UE 5.3](https://nfrechette.github.io/2023/09/17/acl_in_ue); [UE docs: ACL](https://dev.epicgames.com/documentation/unreal-engine/animation-compression-library-in-unreal-engine)
- ACL 1.0 released July 2018; 1.1 claimed cutting animation costs roughly in half vs the previous version ("Smaller, faster: ACL lets you cut your animation costs in half") — [ACL 1.0.0](https://nfrechette.github.io/2018/07/21/acl_v1.0.0); [ACL 1.1.0](https://nfrechette.github.io/2018/09/08/acl_v1.1.0/)

### Inferences
- **Quick-to-medium win**: compress glTF clips at cook time (flatbuffers payload holds the ACL compressed buffer), decompress per-joint at sample time in `Animator::update`. Brings memory reduction and deterministic error bounds; also required groundwork if motion matching uses large mocap sets. Check vcpkg port availability (`acl` may need FetchContent like Slang).

### Gaps
- No up-to-date ACL 2.x benchmark numbers (compression ratio, decompress ns/joint) were retrieved.

---

## 10. Fit to quat-3d and recommended priorities

### Takeaway
Highest value-for-effort: inertialization, foot IK, ACL, shell fur and Jolt-based character cloth/ragdolls (all
reuse existing systems). Medium: motion matching, hair cards with a hair shading model, Jolt GPU hair evaluation.
Long-term: full compute-rasterised strand hair with deep-opacity/voxel shadowing and OIT, GPU XPBD cloth, learned
controllers.

### Cited Findings
- Jolt 5.6 GPU hair sim has a Vulkan compute backend and skinned-root support — [Jolt ReleaseNotes.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/ReleaseNotes.md)
- Shipped strand hair is memory/compute intensive and offered as a toggle even on current consoles — [Microsoft Developer (Veilguard)](https://developer.microsoft.com/en-us/games/articles/2024/12/bioware-dragon-age-the-veilguard-strand-hair-technology/); [TechRadar](https://techradar.com/gaming/consoles-pc/dragon-age-the-veilguards-hair-technology-features-50-000-individual-strands-per-character)
- Every strand system LODs to cards/meshes at distance — [UE docs: groom LOD](https://dev.epicgames.com/documentation/unreal-engine/setting-up-level-of-detail-for-grooms-in-unreal-engine?lang=en-US)

### Inferences
| Technique | Fit with quat-3d | Requirements | Priority |
|---|---|---|---|
| Inertialization | Animator/state machine; pure math, headless-testable | none | Quick win |
| Two-bone IK + foot placement (Jolt raycasts) | After blend tree, before world transforms | Jolt queries | Quick win |
| ACL compression | Cook pipeline + `Animator` sampling | ACL dep (MIT) | Quick win / medium |
| Shell + fin fur | Instanced shells off compute-skinned verts; alpha-tested into GBuffer | instancing (R9.1) | Quick win |
| Jolt ragdolls (death, powered hit reactions) | `SkeletonMapper` + `DriveToPoseUsingMotors` | Jolt ≥5.6 | Medium |
| Jolt soft-body character cloth | Skinning constraints; CPU sim, upload verts | mesh cooking for sim | Medium |
| Hair cards + Marschner-style shading model | GBuffer shading-model ID; TAA-friendly dithering | anisotropic tangents | Medium |
| Motion matching | New animator node; needs inertialization + mocap | feature DB cooker | Medium |
| Jolt GPU hair sim (Cosserat) | Evaluate device/VMA sharing with render graph | Jolt ≥5.6/5.7, Vulkan compute | Medium (evaluate) |
| Compute-rasterised strand hair (visibility buffer, strand-space shading, DOM/voxel shadows, OIT/AA) | Separate render-graph branch after lighting, before TAA | 64-bit atomics (feature-check), large memory | Long-term |
| GPU XPBD cloth | Compute pass; Jolt CPU as reference | graph colouring | Long-term |
| Learned MM / DReCon | Needs ML training pipeline | inference runtime | Long-term / research |

### Gaps
- No quantitative cost data (ms on a reference GPU) could be retrieved for any of the strand systems in this session; the report writer should not invent budgets.
- Network policy blocked `www.ea.com`, `jrouwe.github.io` and `www.advances.realtimerendering.com`; allowing them would permit reading the Frostbite blog, Jolt HTML docs (soft body/hair pages) and SIGGRAPH Advances PDFs in full.
