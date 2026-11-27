# Real-time Destruction, Fracture, GPU Particles and Fluid/Smoke VFX (2018–2026) — fit to quat-3d

Scope note: research run 2026-11-27. 80.lv and www.ea.com could not be fetched from this session (host blocked by the sandbox network policy), so claims from those sites below come from search-result summaries only and are marked as such. Shipped-game technical details for The Finals and Battlefield 6 are thin in public primary sources; this is called out explicitly where it matters.

quat-3d context checked in-repo: `ROADMAP.md` already lists **R10.3 GPU particles** ("Compute-simulated emitters (spawn, integrate, collide with depth, sort) drawn instanced"); R10.1 GPU clustered light culling shipped in cycle 1 and the forward transparent pass (`src/renderer/transparent_pass.cpp`) already binds the clustered light lists (binding 14..17). Physics is a single `src/physics/physics_world.cpp` over Jolt; networking lives in `src/net/` (replication, prediction, character_step). There is no destruction/fracture code today.

---

## 1. Pre-fracture and destruction middleware (Voronoi pre-fracture, NVIDIA Blast, Unreal Chaos, Geo-Mod 2.0)

### Takeaway
The production norm is **offline pre-fracture into chunk hierarchies + a runtime support/connectivity graph with per-bond health**, not runtime mesh cutting. NVIDIA Blast (open source, rigid pre-fractured destruction with a support graph, island detection and a stress solver) is the clearest reference design to copy or vendor; Unreal Chaos adds a hierarchical "cluster" model driven by spatial *fields* (strain, anchor, sleep/disable).

### Cited Findings
- **NVIDIA Blast** is "designed to support rigid body, pre-fractured destruction" — [Omniverse Blast SDK docs](https://docs.omniverse.nvidia.com/kit/docs/blast-sdk/latest/docs/api/introduction.html); [Blast SDK index](https://docs.omniverse.nvidia.com/kit/docs/blast-sdk/latest/index.html).
- Blast's **support graph** decides how chunks group into actors: nodes = support chunks, edges = bonds; "Blast performs island detection on the support graph to find all groups of support chunks that are connected by unbroken bonds", and each disconnected island becomes a new actor — [Blast 1.1 API intro](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pageintroduction.html); [NvBlastSupportGraph reference](https://nvidia-omniverse.github.io/PhysX/blast/_build/docs/blast-sdk/latest/struct_nv_blast_support_graph.html).
- Blast damage is "modeled by a simple health value associated with the bonds and chunks in the support graph" — [Blast 1.1 Low-Level API (NvBlast)](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pagellapi.html).
- Blast ships a **stress solver extension** (NvBlastExtStress) for structural load/collapse on the bond graph — [Blast Stress Solver docs](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pageextstress.html).
- Blast source: original repo github.com/NVIDIAGameWorks/Blast (referenced by NVIDIA's authoring docs); it has authoring tools (BlastTool) and a UE4 plugin — [Blast authoring intro](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/authoring_docs/Blast_Intro.html); [BlastTool quick start](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/authoring_docs/BlastTool_QuickStart.html); still maintained in Omniverse as an extension — [Omniverse Blast Destruction extension](https://docs.omniverse.nvidia.com/extensions/latest/ext_blast.html).
- **Unreal Chaos Destruction**: destruction starts from a **Geometry Collection** asset built from one or more static meshes; **clustering** lets artists "initialize sets of geometry as a single rigid body, then dynamically break the objects during the simulation" — [Chaos Destruction overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/destruction-overview); [Cluster Geometry Collections guide](https://dev.epicgames.com/documentation/ja-jp/unreal-engine/cluster-geometry-collections-user-guide-in-unreal-engine).
- Chaos **Fields** use a region of space to control simulation properties; main types are Anchor fields (set geometry-collection bones in the volume to Static), Strain/Force fields, Sleep/Disable fields; a cluster breaks when applied *external strain* exceeds its internal strain threshold — [Chaos Fields user guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-fields-user-guide-in-unreal-engine); [Physics Field reference](https://dev.epicgames.com/documentation/unreal-engine/reference-guide-for-physics-field-in-unreal-engine).
- Houdini has a Chaos integration path (Houdini-authored fracture → UE Geometry Collections) — [SideFX: Houdini Chaos integration](https://www.sidefx.com/docs/houdini/unreal/chaosintegration.html).
- Community note: Chaos destruction "Documentation is currently lacking" (forum, not official) — [UE forum thread](https://forums.unrealengine.com/t/managing-geometry-collections-with-chaos-physics-field-responses-and-controlled-destruction/2557942).
- **Red Faction: Guerrilla (Geo-Mod 2.0, 2009)** moved away from Geo-Mod 1's real-time CSG to **pre-broken meshes** responding to stress and kinetic impact, with buildings collapsing after lower supports are destroyed; terrain is not destructible in Guerrilla — [Red Faction Wiki: GeoMod 2.0](https://www.redfactionwiki.com/wiki/GeoMod_2.0) (fan wiki); [Wikipedia: Red Faction: Guerrilla](https://www.wikipedia.org/wiki/Red_Faction_Guerilla); [Giant Bomb: destructible environment](https://giantbomb.com/destructible-environment/3015-243/).

### Inferences
- The Blast data model (chunk hierarchy → support chunks → bonds with area/centroid/normal + health → island detection → new actors) maps cleanly onto Jolt: each Blast "actor" = one Jolt body whose shape is a compound of its chunk convex hulls. This is the architecture to adopt even if Blast itself is not vendored (Blast's low-level library is physics-agnostic C; vendoring vs. reimplementing is a build/licence decision — NVIDIA's repo is BSD-3 per background knowledge, unverified here).
- Authoring: quat-3d already imports glTF; the cheapest pipeline is an offline "cook" step that Voronoi-fractures a mesh (seeded points, clipped convex cells; optionally artist-authored in Houdini RBD Material Fracture or Blender's Cell Fracture add-on) and writes a chunk/bond asset. Because chunks are convex Voronoi cells, each chunk's collider is exactly a `JPH::ConvexHullShape`, which is the cheapest Jolt shape for this.
- Interior faces need a separate material slot/UV set ("inside" material) — every pre-fracture pipeline does this; plan the cooked format for it from day one.
- Chaos's "fields" concept (spatial strain/anchor/sleep volumes) is a natural fit for quat-3d's MCP/Lua-driven world building: fields are simple serialisable volumes an AI tool can place.
- **Priority: medium** (pre-fracture + support graph + Jolt compound) — highest gameplay value per unit effort among destruction options.

### Gaps
- Could not verify Havok Destruction's current status/product name (it was historically a separate Havok product; no 2018–2026 primary source found this session).
- No primary GDC source found for Volition's 2009 Geo-Mod 2.0 stress talks (GDC Vault listing only references them: [GDC Vault RFG MP level design](https://gdcvault.com/play/1012330/contactUs)).
- Did not fetch the Blast GitHub repo to confirm current licence and last commit date.

---

## 2. Runtime / voxel destruction and structural collapse (Teardown, The Finals, Battlefield)

### Takeaway
Two shipped models dominate: **voxel-volume runtime destruction** (Teardown — arbitrary cuts, but no structural stress by design) and **server-authoritative pre-fractured building destruction with a structural integrity model** (The Finals, UE5-based). Mesh boolean/CSG at runtime is largely abandoned in shipped AAA titles in favour of pre-fracture or voxels.

### Cited Findings
- **Teardown** (Tuxedo Labs, Dennis Gustafsson) began "as a technology experiment with voxels"; destruction came first and the game was retrofitted around it — [Game Developer: making of Teardown](https://www.gamedeveloper.com/console/video-breaking-down-the-making-of-i-teardown-i-); [Software Engineering Daily podcast, Jan 2025](https://softwareengineeringdaily.com/2025/01/02/teardown-and-voxel-based-rendering-with-dennis-gustafsson/).
- Teardown's destruction does not model structural stability/tension — buildings can be held up by one voxel — and destruction is deliberately limited for playability — [TechRadar](https://techradar.com/gaming/consoles-pc/teardown-devs-top-priority-is-always-physics-the-game-can-come-later).
- Teardown multiplayer: destruction cannot be synced by sending voxel data ("simply too much data"); instead "parts of the network synchronization rely on determinism" — [80.lv interview](https://80.lv/articles/teardown-developer-breaks-down-multiplayer-and-voxel-destruction-tech) (search-summary only; page not fetchable from this session).
- Gustafsson is building a new voxel engine for future projects, not for Teardown — [80.lv](https://80.lv/articles/teardown-creator-is-working-on-a-new-voxel-engine) (search summary).
- **The Finals** (Embark Studios, Dec 2023): destruction runs **server-side**, so every player sees the same result; clients do not run destruction physics — [PC Gamer](https://www.pcgamer.com/i-almost-dont-believe-the-destructibility-in-this-fps-is-real/); [Digital Trends preview](https://www.digitaltrends.com/gaming/the-finals-preview-destruction-will-make-developers-panic/); [Wikipedia: The Finals](https://en.wikipedia.org/wiki/The_Finals).
- The Finals runs on UE5; one less-established source claims it uses Chaos with Voronoi-based rigid fracture — [thegameraven.com](https://thegameraven.com/how-is-the-finals-unique-in-its-gameplay-and-physics/) (low confidence; Embark has not published a technical breakdown I could find).
- The Finals later added a building-integrity ("Strain") model and, in an Oct 20 2026 update, an intermediate deformation stage (tilted floors, sagging sections) before collapse; an earlier season added "Smooth Destruction" letting collapsing structures damage other buildings — [playfront.de](https://playfront.de/en/the-finals-zerstoerungs-update-neue-physik-ebene-ab-20-oktober/); [ixbt.games, Oct 2026](https://ixbt.games/en/news/2026/10/05/roman-smirnov-pokazal-ulucsennoe-razrusenie-okruzeniia-v-the-finals-suter-avtorov-arc-raiders-polucit-bolsoe-obnovlenie.html) (press, secondary).
- **Battlefield 4 "Levolution"** large events are presented in a GDC 2014 session on "destruction, levolution and the new dynamic weather system" — [GDC Vault: Battlefield 4: Creating a More Dynamic Battlefield](https://gdcvault.com/play/1020470/contactUs). Player commentary says big Levolution events are scripted triggers rather than emergent physics (forum, unverified) — [EA forums](https://forums.ea.com/discussions/battlefield-franchise-discussion-en/the-power-of-a-game-engine/6013404).
- **Battlefield 6 (2025)**: EA says the teams made "part-based destruction" and debris-spawning systems more extensible and reworked how destruction *events* are networked (bandwidth + priority) so simultaneous explosions stay in sync in 64-player matches — [EA: How Battlefield 6 redefined destruction](https://www.ea.com/en/news/how-battlefield-6-redefined-destruction) (search summary only; page not fetchable from this session).

### Inferences
- For quat-3d, voxel destruction à la Teardown implies a whole different renderer (voxel ray-marching) and world representation; it is **long-term / out of scope** unless a voxel game mode is a product goal. The pre-fracture + support-graph model fits the existing mesh/GBuffer pipeline.
- Structural collapse = support graph + anchors (chunks touching static world/"foundation" flagged as anchors) + periodic connectivity check: any island no longer connected to an anchor becomes dynamic. That alone gives Red Faction-style "knock out the supports" behaviour; a stress solver (Blast ExtStress-style, or The Finals-style integrity value) is a later refinement.
- Battlefield's "part-based destruction" + scripted Levolution suggests a pragmatic hybrid: author big set-piece collapses as scripted/baked sequences (Lua-triggered), and use physical pre-fracture for small/medium props.

### Gaps
- No primary Embark technical talk/blog on The Finals' destruction architecture (tick rate, replication format, client prediction of debris) was found; all claims are press-level.
- No primary Frostbite/DICE talk on destruction networking (2018–2026) found this session.

---

## 3. How Jolt can support destruction

### Takeaway
Jolt has no built-in fracture system, but it has the needed primitives: `MutableCompoundShape` (runtime add/remove of sub-shapes), `StaticCompoundShape`, convex hulls, constraints, islands/sleeping, and opt-in cross-platform determinism. Breakable joints must be implemented by the application (read constraint impulses, disable over threshold).

### Cited Findings
- `MutableCompoundShape`: "A shape containing other shapes. This shape can be constructed/changed at runtime and trades construction time for runtime performance" — [Jolt docs](https://jrouwe.github.io/JoltPhysics); `StaticCompoundShape` prohibits modification after creation — [jolt-jni shape guide](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/shape.html).
- Recent Jolt releases added inserting a sub-shape at a given index in a `MutableCompoundShape`, a `Clone()` to copy before modifying, and a fix for bounding-box update when removing the last sub-shape — [Jolt release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html).
- Downstream warning (Godot-Jolt binding, not Jolt core): changing a body's compound after it is in the scene triggered a slow full rebuild in that integration — [godot-jolt issue #660](https://www.Github.com/godot-jolt/godot-jolt/issues/660).
- Jolt's samples list has no destruction/fracture or breakable-constraint sample; relevant ones are sleeping visualisation, "160 Ragdolls in a Pile", "Funnel Test (1000 random shapes)", "Multithreaded Island Simulation" — [Jolt Samples.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/Samples.md).
- Third-party engine (Engine²) built breakable constraints on top of Jolt: constraint marked broken/disabled when torque exceeds a threshold — [EngineSquared ConstraintSettings](https://enginesquared.github.io/EngineSquared/structPhysics_1_1Component_1_1ConstraintSettings.html). The general literature pattern is removing constraints whose impulses/displacements exceed a threshold — [Li et al. 2018](https://cal.cs.umbc.edu/Papers/Li-2018-ERB/Li-2018-ERB.pdf).
- Determinism: Jolt is deterministic on the same binary/platform by default; cross-platform determinism needs `CROSS_PLATFORM_DETERMINISTIC` (≈8% cost per the summarised docs), avoiding platform-varying std functions; `JPH_ENABLE_DETERMINISM_LOG` aids debugging — [Jolt Build README](https://jrouwe.github.io/JoltPhysics/md__build_2_r_e_a_d_m_e.html); [raw README](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Build/README.md). Release notes record fixed cross-platform determinism bugs (ARM64 vs x64 double precision; macOS std heap differences) — [Jolt release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html).

### Inferences
- Recommended mapping: one **intact** destructible = one Jolt body with a `StaticCompoundShape` of chunk hulls (fast, built offline/at load). On break, rebuild per island: each new island gets a fresh body with a compound of its chunks (or a single `ConvexHullShape` if one chunk). Prefer creating new bodies over mutating a live `MutableCompoundShape` when islands split; use `MutableCompoundShape` only for "chip off a piece" cases where the parent keeps most chunks.
- Breakable constraints (for glued props, hinges, bridges): after each fixed step, read the accumulated constraint lambda (Jolt constraints expose total-lambda getters per constraint part — verify exact API names in `Jolt/Physics/Constraints/*.h`) and `SetEnabled(false)` over threshold. Integrate into the existing fixed-timestep loop in `physics_world.cpp`.
- Use Jolt `ContactListener::OnContactAdded` impulse estimates to feed bond damage (Blast's "impact damage" pattern).
- Only enable `CROSS_PLATFORM_DETERMINISTIC` if destruction will be lock-step/deterministic across mixed client platforms; with a server-authoritative model (Section 4) it is unnecessary.

### Gaps
- Could not confirm exact Jolt API names for constraint total lambda/impulse (`GetTotalLambdaPosition` etc.) from a fetched source; verify in Jolt headers before design.
- No published Jolt performance numbers for frequent compound rebuilds.

---

## 4. Networking destruction: deterministic vs. authoritative server

### Takeaway
Shipped approaches: **server-authoritative simulation with event/state replication** (The Finals; Battlefield's networked destruction events with bandwidth/priority management) and **determinism-assisted sync** (Teardown MP, because raw voxel state is too large). For quat-3d's existing server-replication + prediction stack, replicate *discrete destruction events* (bond breaks / chunk IDs) reliably and treat debris motion as server-authoritative-but-cosmetic.

### Cited Findings
- The Finals: destruction runs on Embark-controlled servers, clients don't simulate destruction physics, guaranteeing identical results for all players — [PC Gamer](https://www.pcgamer.com/i-almost-dont-believe-the-destructibility-in-this-fps-is-real/); [Digital Trends](https://www.digitaltrends.com/gaming/the-finals-preview-destruction-will-make-developers-panic/); Embark described it as "a developer's Holy Grail" — [GamesBeat](https://gamesbeat.com/embark-studios-the-finals-debuts-on-pc-and-consoles/).
- Teardown MP relies partly on determinism rather than sending voxel data — [80.lv](https://80.lv/articles/teardown-developer-breaks-down-multiplayer-and-voxel-destruction-tech) (search summary).
- Battlefield 6 reworked network behaviour of destruction events with bandwidth and priority optimisation for 64-player sync — [EA](https://www.ea.com/en/news/how-battlefield-6-redefined-destruction) (search summary).
- Jolt determinism properties (same-binary by default; cross-platform opt-in) — [Jolt Build README](https://jrouwe.github.io/JoltPhysics/md__build_2_r_e_a_d_m_e.html).

### Inferences
- Concrete design for quat-3d (`src/net/replication.cpp`):
  1. **Gameplay-relevant state** (which bonds are broken, which chunks exist, collapse results that affect cover/navmesh) = server-authoritative, sent as compact reliable events: `{destructible_net_id, broken_bond_bitset_delta}`. Pre-fracture makes this cheap: the topology is shared content, only bit flips travel.
  2. **Large dynamic debris** that blocks movement: server-simulated, replicated as rigid-body snapshots with interest management/priority (Battlefield-style priority), interpolated on clients.
  3. **Small cosmetic debris & particles**: client-only, seeded from the event (deterministic seed), never replicated.
  - Clients can *predict* local impact effects (particles, decals, sound) immediately and apply the authoritative break when it arrives.
- Full-determinism lockstep for destruction is only attractive for voxel-scale state (Teardown); it would force `CROSS_PLATFORM_DETERMINISTIC` and strict input sync — not recommended given the existing snapshot/prediction architecture.
- Navmesh: destruction events should trigger local navmesh tile rebuilds (quat-3d has nav/NPC work in flight) — inference.

### Gaps
- No public numbers (tick rate, bandwidth per destruction event) for The Finals or BF6.

---

## 5. Debris management (pooling, sleeping, merging, LOD, GPU debris)

### Takeaway
Primary sources on debris budgets are scarce; the established techniques (aggressive sleeping, lifetimes with fade/sink, pooling, demoting small pieces to non-colliding or GPU-only particles, Chaos "Sleep/Disable" fields) are well known but mostly documented only in engine docs.

### Cited Findings
- Chaos provides **Sleep/Disable fields** for controlling simulated pieces spatially — [Chaos Fields user guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-fields-user-guide-in-unreal-engine).
- Jolt simulates in islands distributed across threads and puts bodies to sleep (samples: "Multithreaded Island Simulation", "Ragdolls in a Pile (Sleeping Visualization)") — [Jolt Samples.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/Samples.md).
- Battlefield 6 made "debris spawning systems" more extensible and tied to gameplay systems — [EA](https://www.ea.com/en/news/how-battlefield-6-redefined-destruction) (search summary).

### Inferences
- Tiered debris for quat-3d:
  - Tier A (gameplay chunks, Jolt bodies, server-authoritative): hard cap per region; on cap, oldest sleeping piece is removed or frozen into a static compound.
  - Tier B (cosmetic chunks, Jolt bodies on a non-replicated "DEBRIS" layer colliding only with STATIC): short lifetime, sleep→fade/sink→return to pool.
  - Tier C (dust/gravel): GPU particles (R10.3) emitted at break surfaces, depth-buffer collision only, zero CPU cost.
- Render all debris chunks through the existing GPU-instanced mesh path (`src/scene/instanced_mesh.cpp`) — chunk meshes of one destructible share a vertex buffer, so a break does not create per-chunk draw calls.
- Add a `DEBRIS` object layer to the CLAUDE.md layer list (currently STATIC/DYNAMIC/CHARACTER/TRIGGER/SENSOR).

### Gaps
- No primary-source debris budget numbers (bodies per frame) from shipped titles found.

---

## 6. GPU particle systems (compute emit / simulate / sort / render)

### Takeaway
The canonical GPU particle architecture — dead/alive index lists, emit and simulate compute passes, indirect-args written by compute, optional bitonic/radix sort, depth-buffer collisions, indirect instanced draw — has been stable since ~2014 (AMD "Holy Smoke", Wicked Engine 2017) and is exactly what R10.3 describes. Modern engines (Niagara) add SDF collisions and (experimental) HW-ray-traced collisions.

### Cited Findings
- **Wicked Engine (2017)** replaced its CPU particle system with a GPU one: resource init with indirect-argument buffers, emit + simulate compute passes, draw count of alive particles via DrawIndirect with args written by the simulation shader; source code linked — [Wicked Engine: GPU-based particle simulation](https://wickedengine.net/2017/11/gpu-based-particle-simulation).
- Wicked Engine (2018) inserted a GPU **SPH** fluid step between emission and integration in the same particle system — [Wicked Engine: scalable GPU fluid simulation](https://wickedengine.net/2018/05/scalabe-gpu-fluid-simulation).
- **AMD "Holy Smoke: Faster Particle Rendering Using DirectCompute" (Gareth Thomas)**: depth-buffer collisions (project particle to screen, read depth, compare in view space, respond with G-buffer normals), GPU **bitonic sort**, soft-particle depth fade, tiled rendering of particles; notes depth collisions fail for off-screen/occluded geometry — [SlideShare deck](https://www.slideshare.net/slideshow/holy-smoke-faster-particle-rendering-using-direct-compute-by-gareth-thomas/35656271); AMD sample [GPUParticles11](https://gpuopen.com/learn/gpuparticles11-directx-11-sdk-sample/).
- **Niagara**: depth-buffer GPU collision is "a low-cost, but also low-accuracy solution"; UE also offers GPU distance-field collisions and "GPU Ray Traces (Experimental)" — [Niagara GPU raytracing collisions docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/gpu-raytracing-collisions-in-niagara-for-unreal-engine). Niagara's GPU SDF collisions depend on UE's global distance field (turning off DFAO in 5.7 reportedly disabled it; split-screen bug evaluates only view 0) — [UE forum 2026](https://forums.unrealengine.com/t/potential-wrong-branch-condition-about-dfao-and-niagara-gpu-sdf-collision/2712960); [UE-366361](https://issues.unrealengine.com/issue/UE-366361).
- CryEngine published GPU fluid dynamics for particles — [CryEngine blog](https://www.cryengine.com/news/looking-ahead-gpu-fluid-dynamics-for-particles).

### Inferences
- R10.3 implementation plan fitting quat-3d conventions (render graph, sync2, descriptor buffer, Slang):
  1. Persistent SSBOs: particle pool (SoA or packed), alive lists ×2 (ping-pong), dead list, counters, indirect args — all VMA, double-buffered where CPU-visible.
  2. Passes in the graph *after GBuffer depth is available* and before TransparentPass: `ParticleEmit` (compute; emitter params via push constants/ParameterBlock) → `ParticleSimulate` (compute; integrate, forces, depth-buffer collision using GBuffer depth+normal, compact alive list, write indirect args) → `ParticleSort` (compute; only for alpha-blended emitters) → draw in TransparentPass with `vkCmdDrawIndirect` (camera-facing quads generated in vertex shader from `SV_VertexID`; mesh-shader path optional).
  3. Determinism/testing: keep a CPU reference integrator for a headless GoogleTest (CLAUDE.md "GPU compute first" rule); GPU test compares N steps within tolerance.
- Collision progression: depth-buffer (quick win, needs no new data) → SDF (requires a global/brick SDF of the scene; medium, shares infrastructure with GI/AO if quat-3d ever builds one) → ray-traced (optional, behind `VK_KHR_ray_query` feature check).
- Destruction tie-in: Section 5 Tier C debris and impact dust are GPU particle emitters spawned from break events.
- Sorting: for ≤~64k sorted particles a single-workgroup-chunked bitonic sort is simple; above that a GPU radix sort (e.g. AMD FidelityFX Parallel Sort — background knowledge, not fetched) scales better.

### Gaps
- Did not retrieve Unity VFX Graph or Frostbite/Destiny particle-lighting primary docs this session.
- No 2020s GDC talk on GPU-driven particles was fetched (search surfaced only 2014–2017 material); later refinements (e.g. mesh-shader particle rendering, wave-intrinsic compaction) are not sourced here.

---

## 7. Particle lighting, soft particles and transparency (OIT)

### Takeaway
Light particles by reading the **same clustered light lists** the transparent pass already uses (quat-3d's R10.1 makes this nearly free); use depth-fade soft particles; and offer **Weighted Blended OIT** (McGuire & Bavoil, JCGT 2013) as the unsorted path for dense smoke/sparks, keeping sorted alpha for hero effects.

### Cited Findings
- **Weighted Blended OIT** (McGuire & Bavoil, JCGT 2013) changes the compositing operator to be order-independent, enabling a streaming single-pass approach; the key contribution is a view-depth-based weight function; it needs only MRT + float render targets and "avoids sorting artifacts and popping for particle systems" — [JCGT paper](https://www.jcgt.org/published/0002/02/09/paper.pdf); [NVIDIA WBOIT sample](https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/opengl_samples/weightedblendedoitsample.htm); practical analysis by MJP — [therealmjp: Weighted Blended OIT](https://therealmjp.github.io/posts/weighted-blended-oit); implementation tutorial — [LearnOpenGL WBOIT](https://learnopengl.com/Guest-Articles/2020/OIT/Weighted-Blended).
- McGuire & Mara's phenomenological scattering transparency (2016/17) extends this and further reduces particle popping (diffusion/refraction approximations) — [NVIDIA Research PDF](https://research.nvidia.com/sites/default/files/pubs/2016-02_A-Phenomenological-Scattering/McGuire2016Transparency.pdf).
- Layered/more accurate WBOIT variants exist (Friederichs et al.) — [GI 2021 paper](https://graphicsinterface.org/wp-content/uploads/gi2021-22.pdf).
- NVIDIA overview of transparency vs. translucency rendering (Dunn & Bavoil) — [NVIDIA developer article](https://developer.nvidia.com/content/transparency-or-translucency-rendering).
- Soft particles via depth fade are part of the AMD GPU particle pipeline — [Holy Smoke deck](https://www.slideshare.net/slideshow/holy-smoke-faster-particle-rendering-using-direct-compute-by-gareth-thomas/35656271).

### Inferences
- quat-3d's transparent pass already binds clustered lights (bindings 14..17); particle shaders can `import lib/clustered_lights.slang` and evaluate per-vertex or per-particle (cheaper, standard for smoke) lighting. Per-pixel lighting is reserved for large/hero sprites.
- WBOIT integrates as two extra render targets (RGBA16F accum + R8/R16F revealage) in the transparent pass plus a fullscreen composite; it interacts with TAA (stable since order-independent) — a good fit. Sorted alpha remains for glass (which already uses scene-colour transmission).
- Shadowed/volumetric lighting of particles (self-shadowing smoke, particles receiving CSM shadows) is a medium-term add: sample the cascaded shadow map per particle in the simulate pass.

### Gaps
- No primary source fetched on clustered particle lighting in a shipped engine (Frostbite/Destiny particle lighting talks not located).

---

## 8. Fluids, smoke and fire: grids, sparse grids, SPH/PBF, flipbooks

### Takeaway
For games, **baked flipbooks / VDB-style caches from offline or EmberGen sims are still the cheapest high-quality path**; real-time in-game simulation is either small dense 3D grids (Stam stable fluids on GPU, GPU Gems 3 era) for localized smoke/fire, or particle-based (SPH/PBF) for liquids. JangaFX's sparse GPU solver (≈1B voxels interactive on top-end GPUs, 2025–26) shows the ceiling but is a DCC tool, not an in-game runtime.

### Cited Findings
- **Stable Fluids** (Stam, SIGGRAPH 1999) — unconditionally stable semi-Lagrangian advection + pressure projection — [Stam PDF](https://pages.cs.wisc.edu/~chaol/data/cs777/stam-stable_fluids.pdf). GPU implementation: Harris, *GPU Gems* ch. 38 (2D, textures as grids) — [GPU Gems ch38](https://developer.nvidia.com/gpugems/GPUGems/gpugems_ch38.html); 3D real-time simulation + volume rendering of smoke/fire/water in *GPU Gems 3* ch. 30 — [GPU Gems 3 ch30](https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-30-real-time-simulation-and-rendering-3d-fluids).
- **Position Based Fluids** (Macklin & Müller, SIGGRAPH 2013, ACM TOG 32(4):104) integrates an iterative density solver into PBD — [SIGGRAPH history](https://history.siggraph.org/?p=108378); [dblp](https://dblp.uni-trier.de/rec/journals/tog/MacklinM13.html). **Unified Particle Physics for Real-Time Applications** (Macklin, Müller, Chentanez, Kim, TOG 2014) couples fluids, cloth, rigid bodies via constraints in one particle framework (basis of NVIDIA FleX — association from background knowledge) — [paper PDF](https://matthias-research.github.io/pages/publications/flex.pdf).
- Wicked Engine shipped GPU SPH inside its particle system — [Wicked Engine 2018](https://wickedengine.net/2018/05/scalabe-gpu-fluid-simulation).
- FLIP for CPU and GPU (2024 paper) — [arXiv 2404.01931](https://arxiv.org/pdf/2404.01931).
- **EmberGen (JangaFX)**: GPU volumetric fluid sim; EmberGen 1.x dense max ≈400M voxels; EmberGen 2.0 roadmap adds a **sparse** solver restricting work to active voxels — ≈200M active voxels on 6 GB, ≈2B on 48 GB, "effectively a 8,192 × 8,192 × 8,192 adaptive simulation domain"; also sim caching, reworked GPU particles, a path tracer; min spec GTX 1060 — [CG Channel, Jan 2025](https://www.cgchannel.com/2025/01/check-out-the-new-features-due-in-embergen-2-0); [CG Channel EmberGen 1.1](https://www.cgchannel.com/2024/01/jangafx-releases-embergen-1-1). A demo showed the sparse solver interactively simulating/rendering 1B voxels on an RTX PRO 6000 Blackwell — [80.lv](https://80.lv/articles/upcoming-embergen-s-sparse-solver-handles-one-billion-voxels-in-real-time) (search summary).

### Inferences
- Priority order for quat-3d:
  1. **Flipbook sprites** (quick win once R10.3 lands): 2D atlas + motion-vector flipbook blending (standard EmberGen/Houdini export) rendered by the GPU particle renderer, lit via clustered lights with normal-mapped/6-way lightmaps (6-way lighting is the current standard for baked smoke in UE/Unity — background knowledge, not sourced here).
  2. **Baked 3D volume caches** (medium): sparse brick textures (NanoVDB-like) ray-marched in a compute pass, composited before/with the transparent pass; good for hero explosions.
  3. **Local real-time 3D grid** (medium/long): a 64³–128³ GPU stable-fluids box (advect → divergence → Jacobi/multigrid pressure → project) for interactive smoke/fire that reacts to characters/explosions; ray-marched with light marching against the CSM. Fits the render graph naturally as compute passes.
  4. **PBF/SPH liquids** (long-term): spatial hash grid on GPU, reuses the particle system's buffers; screen-space fluid rendering (depth smoothing) required for a liquid surface look.
- Volume rendering cost is dominated by ray-march steps × light-march steps; froxel-based volumetric fog (if quat-3d adds it under R8) can share the shadow/light sampling path with smoke volumes.

### Gaps
- No 2018–2026 shipped-game primary source on in-game (not offline) 3D smoke grids was retrieved this session (e.g., examples like UE Niagara Fluids or Ghost of Tsushima-era techniques not verified).
- EmberGen 2.0 release status at 2026-11 not confirmed.

---

## 9. Recommended priority for quat-3d (synthesis)

### Takeaway
Start with **R10.3 GPU particles** (already on the roadmap) plus soft particles, clustered particle lighting and WBOIT; then **pre-fracture destruction** (Blast-style support graph on Jolt compounds, server-authoritative bond-break events); then baked/real-time volumetrics. Voxel destruction, runtime mesh cutting and full fluid sims are long-term.

### Cited Findings
- Roadmap already defines R10.3 GPU particles (spawn, integrate, collide with depth, sort, instanced draw) — quat-3d `ROADMAP.md` (local).
- Canonical GPU particle pipeline sources — [Wicked Engine](https://wickedengine.net/2017/11/gpu-based-particle-simulation); [AMD Holy Smoke](https://www.slideshare.net/slideshow/holy-smoke-faster-particle-rendering-using-direct-compute-by-gareth-thomas/35656271).
- Pre-fracture/support-graph reference design — [Blast API intro](https://docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pageintroduction.html).
- Server-authoritative destruction precedent — [PC Gamer on The Finals](https://www.pcgamer.com/i-almost-dont-believe-the-destructibility-in-this-fps-is-real/).

### Inferences
| Technique | Cost / complexity | HW/API needs | Fit to quat-3d | Priority |
|---|---|---|---|---|
| GPU particles (emit/sim/compact/indirect draw) | Medium (≈4 compute passes + draw) | Vulkan 1.3 compute, indirect draw | Direct match to R10.3, render graph, Slang | **Quick win / next** |
| Depth-buffer particle collision + soft particles | Low | GBuffer depth+normal | Data already exists | **Quick win** (part of R10.3) |
| Clustered particle lighting | Low | R10.1 clusters | Transparent pass already binds clusters | **Quick win** |
| Weighted Blended OIT for particles | Low–medium | 2 extra float RTs | TAA-friendly, transparent pass | **Quick win** |
| Flipbook (motion-vector) smoke/fire | Low | Texture atlas | Needs R10.3 | **Quick win** |
| Pre-fracture + support graph + Jolt compounds | Medium–high (cook tool, runtime graph, Jolt glue) | None special | Jolt compound/convex hulls, instancing | **Medium** |
| Breakable Jolt constraints | Low | — | physics_world fixed step | **Medium (with above)** |
| Networked destruction events (bond-bitset deltas) | Medium | — | src/net replication | **Medium** |
| Debris tiers + pooling + GPU dust | Medium | — | Instancing + R10.3 | **Medium** |
| Structural stress solver (Blast ExtStress-like) | High | — | Builds on support graph | **Long-term** |
| SDF particle collision | High (needs scene SDF) | Compute, 3D textures | Shares infra with future GI/AO | **Long-term** |
| Baked sparse volume caches (VDB-like) | Medium–high | 3D textures/bricks | Compute ray-march pass | **Medium/long** |
| Real-time local 3D smoke grid | High | Compute | Render-graph compute | **Long-term** |
| PBF/SPH liquids | High | Compute | Reuses particle buffers | **Long-term** |
| Voxel (Teardown-style) destruction | Very high (new world/renderer) | — | Conflicts with mesh/GBuffer pipeline | **Out of scope unless product goal** |
| Runtime mesh boolean cutting | High, fragile | — | Industry moved away (Geo-Mod 2.0) | **Avoid** |

- MCP/Lua angle: expose destruction "fields" (strain/anchor/sleep volumes) and particle emitter descriptors as data so `quat-mcp` tools can place and tune them; validate all parameters per CLAUDE.md remote rules.

### Gaps
- No cost measurements on quat-3d hardware; all costs above are qualitative engineering estimates.
