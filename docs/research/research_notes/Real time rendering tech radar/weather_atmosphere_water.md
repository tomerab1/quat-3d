# Real-time weather, atmosphere and water techniques (2018–2026) for quat-3d

Research notes, 2026-11-27. Scope: volumetric clouds, volumetric fog/froxels, weather systems (rain, snow, wetness, wind, lightning, weather state, time of day), and water/ocean (FFT, shallow water, rendering, buoyancy), with notes on how each fits quat-3d (Vulkan 1.3, Slang, render-graph compute passes, deferred + clustered, TAA, Hillaire-style atmosphere LUTs already present, Jolt).

**Method note / source access:** Several primary hosts were unreachable from this research container (DNS blocked by the session's network policy): `advances.realtimerendering.com`, `www.guerrilla-games.com`, `sebh.github.io`, `gpuopen.com`. Findings from those decks come from search-engine excerpts of the decks, not full reads, so exact numbers inside slide decks (cloud voxel resolutions, Atlas FFT sizes, per-pass ms) are mostly listed as gaps. Items marked **[bg]** are canonical background references whose URL comes from the researcher's prior knowledge and was **not fetched this session**. Treat their details as unverified.

---

## 1. Volumetric clouds: Nubis (2015/2017/2022/2023), Frostbite/Hillaire, Unreal, temporal reprojection costs

### Takeaway
The production baseline is still the 2.5D "Nubis" model: a weather map plus Perlin–Worley/Worley 3D noise, ray-marched at reduced resolution and filled in with temporal reprojection, in about a 2 ms console budget. Guerrilla then moved to voxel clouds with SDF-accelerated marching ("Nubis Evolved" 2022, "Nubis Cubed" 2023) to get flyable, fluid-sim-authored cloudscapes. For quat-3d, which already has Hillaire-style atmosphere LUTs, the natural next step is a Nubis-2017/Hillaire-2016-style cloud layer: a compute pass at 1/4 resolution with temporal upsampling, plus a cloud shadow map. Voxel/SDF clouds are a long-term item.

### Cited Findings
**Guerrilla / Nubis lineage**
- Nubis 2017 ("Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima Engine", SIGGRAPH 2017 Advances). The slides say the team "improved the performance beyond our 2 ms budget while also adding an authoring component". The 2015 prototype (Horizon Zero Dawn) rendered in under 2 ms on PS4. — [Guerrilla publication page](https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine); [Advances 2017 PDF](https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf)
- "Nubis Cubed" (Andrew Schneider, SIGGRAPH 2023 Advances): a voxel-based cloud renderer and modelling approach built in under six months. From the ground the voxel clouds serve as volumetric skyboxes with a time-of-day cycle, and they hold high frame rates while the player flies on a mount. Listed techniques: ray-march acceleration with compressed signed distance fields, cloud modelling based on fluid simulation, upscaling dense voxel data without memory bottlenecks, faster light sampling, and approximations for dark edges and inner glow. The talk explains why Guerrilla moved away from its 2015/2017 2.5D methods. — [Guerrilla: Nubis Cubed](https://www.guerrilla-games.com/read/nubis-cubed); [Advances 2023 PDF](https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf)
- "Nubis, Evolved" (SIGGRAPH 2022) is the intermediate step and is cited by the 2023 deck. — [Guerrilla: Nubis Evolved](https://www.guerrilla-games.com/read/nubis-evolved)
- An open Vulkan reimplementation of Nubis3 methods (UPenn CIS 565 "Frostnova") uses adaptive ray-march step sizes and Henyey–Greenstein phase functions. It is useful as a reference for Vulkan structure. — [GitHub: CIS5650-Final-Project-Frostnova](https://github.com/YueZhang1027/CIS5650-Final-Project-Frostnova)

**Reduced-step / temporal approaches**
- Studio Gobo (Toft, Bowles, Zimmermann), "Optimisations for Real-Time Volumetric Cloudscapes" (2016): ray marching needs 1/16 of the steps of the traditional method. The paper describes prior approaches that render clouds at reduced resolution and fill the remaining pixels with temporal reprojection. — [arXiv 1609.05344](https://arxiv.org/pdf/1609.05344)

**Frostbite / Hillaire**
- Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite" (SIGGRAPH 2016 PBS course). It covers Rayleigh/Mie sky, sun rendering, and a volumetric cloud layer "similar to [Schneider15]" with weather animation, dynamic lighting and shadowing, and Perlin–Worley noise. Sky/aerial-perspective rendering costs 0.42 ms on Xbox One, with LUT updates using 4th-order scattering. The course frames sky, atmosphere and clouds as "the three main components we need to simulate in order to achieve dynamic time of day and weather conditions". — [EA Frostbite news page](https://www.ea.com/news/physically-based-sky-atmosphere-and-cloud-rendering); [Course PDF](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf)
- Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020, CGF 39(4) pp. 13–22, DOI 10.1111/cgf.14050). It uses simple LUTs (transmittance, multi-scattering, sky-view, aerial-perspective volume) instead of high-dimensional LUTs, plus a new real-time multiple-scattering approximation. Cost is decoupled from screen resolution and scales from mobile to high-end PC through LUT resolution and ray-march sample count. Atmosphere composition can change at runtime without heavy LUT updates. It ships in Unreal Engine (SkyAtmosphere). — [EG Digital Library](https://diglib.eg.org/handle/10.1111/cgf14050); [PDF](https://diglib7.eg.org/bitstream/handle/10.1111/cgf14050/v39i4pp013-022.pdf); [SIGGRAPH 2020 PBS course slides](https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf)
- A 2016 Chalmers MSc thesis, "Convincing Cloud Rendering", implemented dynamic volumetric clouds inside Frostbite's sky module. It is a useful secondary write-up. — [Chalmers PDF](https://publications.lib.chalmers.se/records/fulltext/241770/241770.pdf)
- **[bg]** Hillaire's reference implementation of the EGSR 2020 atmosphere (D3D11, MIT): [github.com/sebh/UnrealEngineSkyAtmosphere](https://github.com/sebh/UnrealEngineSkyAtmosphere)

**Unreal Engine Volumetric Cloud**
- UE's Volumetric Cloud component uses "ray-marching and approximation to simulate cloud rendering for scalable real-time performance across many supported platforms". The cost levers are:
  - Disabling *Ray March Volume Shadow* makes clouds use the cloud shadow map instead of secondary ray marching. This "is usually enough for clouds views from the ground and it should result in a performance boost".
  - *View Sample Count Scale* scales the view tracing sample count.
  - Each multiple-scattering octave adds shader cost: "best to use a single octave".
  
  — [UE docs: Volumetric Cloud component](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine); [properties](https://dev.epicgames.com/documentation/unreal-engine/volumetric-cloud-component-properties-in-unreal-engine?lang=en-US)

**Other shipped data points**
- A PlayStation Blog deep dive on *Ghost of Yōtei* notes that Ghost of Tsushima's engine could not render clouds in front of world geometry. Yōtei's newer engine could, which is evidence that full in-world volumetric clouds are a recent console-generation feature. — [PlayStation Blog: Ghost of Yōtei tech deep dive](https://blog.playstation.com/?p=411201)

### Inferences
- **Fit to quat-3d:** Cloud ray marching is a pure compute pass in the render graph. Its inputs are the weather map (2D), 3D noise textures (128³ shape plus 32³ detail is the commonly used split; not verified this session), the existing transmittance LUT, and the multi-scatter LUT for sun/ambient colour. Its outputs are a low-resolution cloud colour+transmittance target and a cloud shadow map, with a temporal reprojection/upsample pass before composite. The existing TAA history/motion-vector infrastructure can be reused for reprojection. The cloud shadow map can also feed the volumetric fog froxels and the clustered/deferred sun term.
- Slang interfaces fit the "cloud density provider" abstraction well: a 2.5D weather-map density now, and a voxel/SDF density later behind the same interface, which avoids `#ifdef` variants.
- An MCP/AI world-building hook is natural. Weather map painting, coverage and cloud type are data the remote API can expose.
- **Priority:**
  - Medium: 2.5D Nubis/Hillaire-2016 cloud layer with temporal reprojection and a cloud shadow map.
  - Quick win: a cloud shadow map projected on the terrain from the existing procedural clouds, if they are 2D.
  - Long-term: Nubis-Cubed voxel/SDF clouds. They need offline fluid-sim authoring tools and a significant memory/compression design.

### Gaps
- No published per-pass ms figures for Nubis Evolved/Cubed (PS5) were retrievable. The slide decks were blocked, and the excerpts give no numbers.
- Voxel grid resolution, SDF compression format and memory footprint of Nubis Cubed are not confirmed.
- No confirmation from UE docs on the specific temporal reprojection scheme UE Volumetric Clouds use (search found none). It is widely believed to render at reduced resolution with reconstruction, but this is unverified.
- The ~2 ms budget figure is for PS4-era 2.5D clouds. I found no modern desktop-GPU numbers.

---

## 2. Volumetric fog / froxel volumetrics, local fog volumes, light shafts

### Takeaway
Froxel (camera-frustum-aligned voxel) volumetric fog is the industry standard. It was introduced by Wronski (AC4, 2014) and unified with participating-media physics by Frostbite (Hillaire 2015). It is now present in UE, CryEngine, Godot 4 and Flax. It is a sequence of compute passes (inject media → light scattering → integrate) with temporal reprojection and sub-voxel jitter, costing about 1 ms on PS4-class hardware. quat-3d's clustered light grid is the same frustum parameterisation, so froxel fog is a high-value, medium-effort item that shares the light-culling data.

### Cited Findings
- Wronski, "Volumetric Fog: Unified Compute Shader Based Solution to Atmospheric Scattering" (SIGGRAPH 2014 Advances; AC4: Black Flag, Ubisoft Montreal). The GDC 2014 deck lists the volume as 16-bit float RGBA at 160×90×64 or 160×90×128. A write-up reports about 1.1 ms total in AC4, and the PS4 could run it asynchronously. — [SIGGRAPH 2014 slides](https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf); [AC4 GDC 2014 deck](https://bartwronski.com/wp-content/uploads/2014/03/ac4_gdc.pdf); [GDC Vault session](https://www.gdcvault.com/play/1020397/contactUs); cost figure via [VGChartz forum write-up (secondary)](https://gamrconnect.vgchartz.com/post.php?id=6178608)
- Hillaire, "Physically Based and Unified Volumetric Rendering in Frostbite" (SIGGRAPH 2015 Advances). It covers:
  - a cascaded volume representing extinction;
  - voxelization of particles into that extinction volume;
  - a simple volumetric shadow map;
  - froxel integration for sun, ambient and emissive light;
  - validation against the Mitsuba path tracer.
  
  — [EA Frostbite page](https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite); [SlideShare deck](https://www.slideshare.net/DICEStudio/physically-based-and-unified-volumetric-rendering-in-frostbite)
- Unreal Engine Volumetric Fog: the volume textures are relatively low resolution and aligned to the camera frustum. The fog "uses a heavy temporal reprojection filter with a different sub-voxel jitter per frame to smooth out the aliasing". GPU cost is primarily controlled by volume texture resolution, which is set from the Shadow scalability level. — [UE docs: Volumetric Fog](https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine)
- A dated (UE 4.16-era), third-party figure puts UE volumetric fog at about 1 ms on PS4 at High settings and 3 ms on a GTX 970 at Epic settings, which uses 8× more voxels. — [80.lv cheat sheet](https://80.lv/articles/a-cheat-sheet-on-volumetric-fog-in-ue4/) (secondary, old; verify)
- Godot 4 describes its implementation as a "froxel buffer", a large 3D buffer where lighting is computed only per cell and meshes then sample it. It also supports local *FogVolume* nodes, which inject density into the froxel grid. — [Godot: Fog volumes arrive in Godot 4](https://godotengine.org/article/fog-volumes-arrive-in-godot-4)
- Other engines with voxel/froxel volumetric fog include CryEngine ("Voxel-based Volumetric Fog") and Flax Engine. — [CryEngine news](https://www.cryengine.com/news/view/looking-ahead-at-things-to-come-voxel-based-volumetric-fog); [Flax docs](https://github.com/FlaxEngine/FlaxDocs/blob/c7ca0c976936f5203fc441816dcab68a7c4f31f9/manual/graphics/fog-effects/volumetric-fog.md/)
- Naughty Dog presented "Volumetric Effects of The Last of Us: Part Two" (Kovalovs, SIGGRAPH 2020 Talks), a shipped production reference for volumetric effects. — [SIGGRAPH history archive](https://history.siggraph.org/learning/volumetric-effects-of-the-last-of-us-part-two-by-kovalovs)
- An HPG 2023 paper on volumetrics appeared in results ([diglib PDF](https://diglib7.eg.org/bitstream/handle/10.2312/hpg20231136/055-064.pdf)), but its contents were not verified.

### Inferences
- **Fit to quat-3d:** The froxel grid can share the clustered-lighting Z-slicing (exponential depth slices) and light lists, so the scattering pass loops over the same per-cluster light lists. The planned R10.1 GPU clustered culling is a natural dependency.
- The pipeline maps to render-graph compute passes:
  1. Media injection: global height fog, local fog-volume primitives (box/sphere/ellipsoid with noise), and optionally particles.
  2. In-scattering, using shadow maps (CSM), the cloud shadow map and the atmosphere transmittance LUT for sun colour. Hillaire's EGSR 2020 aerial-perspective froxel volume already does a similar job for atmosphere.
  3. Temporal blend with a jittered sample offset.
  4. Front-to-back integration.
  5. Apply in the lighting/transparent passes. Forward-rendered transparents sample the integrated volume.
- Light shafts ("god rays") come for free from shadowed froxel in-scattering. Screen-space radial blur is obsolete for a deferred engine that has froxels.
- Local fog volumes are good ECS data (component: shape, density, albedo, emissive, noise params) and good MCP surface area.
- **Priority:** Medium (high value). Local fog volumes are a quick follow-up once the grid exists.

### Gaps
- No current-generation (RDNA2/Ampere+) ms numbers from primary sources for froxel fog at typical grids (e.g., 160×90×128 or 240×135×128).
- Frostbite 2015 cascade/froxel resolutions and costs were not retrievable (PDF host blocked or not fetched).

---

## 3. Weather systems: rain, snow, wetness/puddles/ripples, wind, lightning, weather state, time of day

### Takeaway
Shipped weather is a set of loosely coupled systems driven by a small global weather state: wetness/snow amount, rain intensity, wind vector, cloud coverage and fog density.
- **Rain:** GPU particles/streaks culled by a top-down occlusion (height) map, plus a wet-surface BRDF modifier and procedural ripple normal maps.
- **Snow:** accumulation as a material blend driven by surface orientation and an occlusion height map. Deformation uses a camera- or player-following heightmap render target (Batman: Arkham Origins → Rise of the Tomb Raider's "deferred deformation").
- **Wind:** a GPU 3D fluid grid (God of War) or vorticle-based simulation (Ghost of Tsushima) feeding grass, cloth and particles.

quat-3d already has compute grass and terrain, so a global wind field texture plus a wetness/snow layer in the GBuffer material evaluation are the quick wins.

### Cited Findings
**Rain and wet surfaces**
- Tatarchuk, "Artist-Directable Real-Time Rain Rendering in City Environments" (SIGGRAPH 2006 Advances, ATI ToyShop demo). It covers view-dependent warped reflections on wet materials and puddles via reflection impostors, and a GPU water-surface simulation for puddle ripples from raindrop splashes. It is the foundational reference. — [Course chapter PDF](https://advances.realtimerendering.com/s2006/Chapter3-Artist-Directable_Real-Time_Rain_Rendering_in_City_Environments.pdf); [Slides](https://advances.realtimerendering.com/s2006/Tatarchuk-Rain.pdf)
- Wronski (AC4 GDC 2014 / Digital Dragons 2014): a procedural rain ripple effect using compute and geometry shaders, a wet-surface material approximation, GPU-driven rain simulation, and "World Ambient Occlusion" (a top-down occlusion technique from Assassin's Creed 3) used for occlusion of rain/sky. — [Digital Dragons 2014 slides post](https://bartwronski.com/2014/05/09/digital-dragons-2014-slides/); [AC4 GDC deck](https://bartwronski.com/wp-content/uploads/2014/03/ac4_gdc.pdf)
- Lagarde's 2012 "Observe Rainy World" / "Water drop" blog series is the canonical background for wet-surface shading. Wetness darkens albedo and raises smoothness, puddles flatten normals, and there are ripples and streaks. Community wet shaders still cite it. — [Gist citing it](https://gist.github.com/eviltak/e04b83ffdb91aa3d477bbfe0ca370da7); **[bg]** original: https://seblagarde.wordpress.com/2012/12/10/observe-rainy-world/
- Wang et al., "Real-time Rendering of Realistic Rain" (Microsoft Research, SIGGRAPH 2006 sketch) is an older academic reference for rain streak appearance. — [MSR](https://www.microsoft.com/en-us/research/?p=156502); [Tech report PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/RealTimeRain_MSTR.pdf)
- fxguide's "Game environments – Part C: making wet environments" is an industry overview of wet-environment art and tech. — [fxguide](https://www.fxguide.com/?p=43362)

**Snow accumulation and deformation**
- Michels & Sikachev, "Deferred Snow Deformation in Rise of the Tomb Raider" (GPU Pro 7, CRC Press 2016, pp. 3–16). Deferred deformation moves the cost from rendering the deformation heightmap to reconstructing depression/elevation during the snow render pass, which avoids prohibitively expensive statically tessellated high-poly snow. It builds on Barré-Brisebois, "Deformable Snow Rendering in Batman: Arkham Origins" (GDC 2014). — [Game Engine Gems DB entry](https://www.gameenginegems.com/gemsdb/article.php?id=1314); [Taylor & Francis chapter](https://www.taylorfrancis.com/books/9780203705179/chapters/10.1201/b22483-18); [SIGGRAPH 2015 Labs talk "Rendering Techniques in Rise of the Tomb Raider"](https://history.siggraph.org/wp-content/uploads/2022/10/2015-Talks-Michels_Labs-R_D-Rendering-Techniques-in-Rise-of-the-Tomb-Raider.pdf)
- Red Dead Redemption 2 snow/mud:
  - No official Rockstar technical talk on snow deformation was found.
  - Press observations say tracks are persistent and differ for on-foot vs horse.
  - Community recreations use a player-following deformation render target.
  - Polycount speculation about meshes/decals is unverified.
  
  — [GamingBolt](https://gamingbolt.com/15-tiny-but-amazing-red-dead-redemption-2-details-you-need-to-see/12); [Community recreation](https://pappaniels.github.io/posts/2025/01/23/Snow-Deformation.html); [GameDev.net thread](https://gamedev.net/forums/topic/700247-snow-physics-in-red-dead-redemption-2/)

**Wind**
- Rupert Renard, "Wind Simulation in God of War" (GDC 2019, Sony Santa Monica). It describes a full 3D GPU fluid simulation for wind, the techniques that make it fast and useful, and what did and did not work. A companion talk, "Interactive Wind and Vegetation in God of War", covers wind fluid simulation, wind motors (emitters), wind receivers, mesh data and compute details. — [GDC Vault: Wind Simulation](https://gdcvault.com/play/1025972/Wind-Simulation-in-God-of); [GDC Vault: Interactive Wind and Vegetation](https://gdcvault.com/play/1026036/Interactive-Wind-and-Vegetation-in); [80.lv summary](https://80.lv/articles/santa-monica-studio-showed-how-interactive-wind-and-vegetation-were-made-for-god-of-war)
- Bill Rockenbeck, "Blowing from the West: Simulating Wind in Ghost of Tsushima" (GDC 2021, Sucker Punch). Wind interacts with cloth, grass and hundreds of thousands of particles on the PS4 GPU, and the team prioritised volume over accuracy. It uses a constant main wind direction with magnitude varied by time-varying Perlin noise, plus "vorticles" (vortex particles) for local disturbance. — [GDC Vault](https://gdcvault.com/play/1027350/Blowing-from-the-West-Simulating); [Game Developer: Using vorticles](https://gamedeveloper.com/design/using-vorticles-to-simulate-wind-in-i-ghost-of-tsushima-i-); [GDC news](https://gdconf.com/news/see-how-ghost-tsushima’s-guiding-wind-came-life-gdc-2021)
- Ghost of Tsushima weather is described as procedural and algorithmic, covering clouds, the sunlight filtering through them, wind and grass sway. No talk detailing the weather state machine was found. — [Gamesradar (secondary)](https://gamesradar.com/sucker-punch-has-put-so-much-thought-into-the-tiniest-details-of-ghost-of-tsushima-that-it-kinda-makes-me-want-to-cry)
- A separate GDC talk covers Ghost of Tsushima's procedural grass (Eric Wohllaib). It is relevant for coupling wind with quat-3d's compute grass. — [GDC Vault](https://gdcvault.com/play/1027214/contactUs)

**Weather as a whole / time of day**
- Frostbite frames sky, atmosphere and clouds as the components needed for dynamic time of day and weather. Their LUT updates (0.42 ms on XB1, 4th-order scattering) are cheap enough to update with weather changes. — [Frostbite 2016 course PDF](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf)
- Hillaire 2020: atmosphere composition can change at runtime without heavy LUT updates. Haze, dust and smog can therefore be weather-driven per frame. — [EG Digital Library](https://diglib.eg.org/handle/10.1111/cgf14050)

### Inferences
- **Architecture for quat-3d:**
  - **Weather state component.** A singleton `WeatherState` (POD) holds coverage, cloud type, precipitation rate and type, wetness, snow cover, wind direction/speed/gustiness, fog density, lightning probability and temperature. It is updated by a system that interpolates toward a target preset over a duration, from a simple state machine or a Lua script.
  - **Consumers.** Every consumer reads this one struct (or a UBO mirror): the cloud pass, fog injection, the atmosphere aerosol params, the rain/snow particle compute, the material wetness/snow terms in the GBuffer or lighting resolve, and the grass wind field.
  - **MCP/Lua.** This gives a single, AI-friendly control surface for MCP ("set_weather {preset, transition_s}") and Lua.
- **Rain:**
  - Compute-particle rain lives in a camera-local box that wraps around the camera.
  - A **top-down depth/occlusion map** of the scene around the camera is rendered once or on movement. Many engines can reuse a shadow-style orthographic pass, and the terrain heightmap already gives most of it. It culls particles under cover and masks wetness and ripple application.
  - Splashes are spawned on depth-buffer hits.
  - Ripples come from a tiled procedural ripple normal texture generated in compute.
  - Wetness modifies GBuffer albedo, roughness and normal, ideally in the deferred lighting resolve or a GBuffer-modify compute pass so every material gets it without shader permutations. This fits the "Slang interfaces, no #ifdef" rule.
- **Snow:**
  - Accumulation blends by world-space normal·up × snow amount × occlusion-map visibility.
  - Deformation uses a toroidally scrolling, player-centred heightmap render target written by compute from character/vehicle contacts (Jolt contact points). It is reconstructed in the terrain shading pass in the deferred-deformation style, or by displacement if terrain tessellation/mesh shaders exist. quat-3d's GPU terrain synthesis already owns heightfields, so the deformation map is a natural extra layer.
- **Wind:** Start with a global wind vector plus a scrolling noise texture sampled by compute grass, trees, cloth and particles (Ghost of Tsushima-style "volume over accuracy"). Upgrade to a camera-centred 3D wind grid with motors (God of War-style) solved in compute (advect + project). Jolt bodies can sample the same field for drag.
- **Lightning:** A procedural branching bolt mesh (mid-point displacement) plus a short-lived directional/point light injected into the clustered light list and the froxel fog. A cloud-interior flash is an emissive term in the cloud march. Audio is delayed by distance. No primary source was found this session (see Gaps).
- **Priority:**
  - Quick wins: global WeatherState plus a Lua/MCP binding; global wind texture feeding grass; the wetness/snow material layer; rain particles with occlusion map.
  - Medium: ripples/puddles (puddle mask from terrain cavity/erosion data, which already exists in quat-3d's erosion sim), snow deformation heightmap, lightning.
  - Long-term: 3D wind fluid grid.

### Gaps
- No primary 2018–2026 talk on rain rendering (e.g., a recent AAA rain deep dive) was retrieved. The well-sourced references are 2006–2014. Recent ones likely exist (e.g., Far Cry, Forza Horizon, Cyberpunk 2077 wet roads) but were not found or verified.
- No primary source on snow accumulation/deformation for God of War (2018) or RDR2. The RDR2 technique is undocumented officially, as far as this search found.
- No primary source found on lightning rendering or on weather state-machine design in shipped titles.
- God of War wind grid resolution/solver and the cost numbers are not public in the retrieved material.

---

## 4. Water: FFT ocean, interactive/shallow-water sims, rivers, water rendering, buoyancy with Jolt

### Takeaway
The production ocean is a Tessendorf FFT heightfield with choppy horizontal displacement, driven by empirical spectra (JONSWAP, often two of them: wind sea plus swell, as in Atlas/WaveWorks 2.0). It is typically multi-cascade and computed in GPU compute. Interactive detail (wakes, explosions, splashes) is layered on as a local dynamic or shallow-water simulation. The surface is shaded with a single-layer water model (absorption/scattering, refraction from a depth+colour copy, reflections via SSR/probes, foam from Jacobian/crest masks). Buoyancy is CPU-side from sampled heights. Jolt has a built-in `ApplyBuoyancyImpulse` per body against a surface plane, so per-body local planes from the CPU-evaluated or read-back water height give cheap, robust coupling.

### Cited Findings
**FFT ocean and spectra**
- Tessendorf's "Simulating Ocean Water" (SIGGRAPH course) introduced the oceanographic approach to FFT-synthesised waves and is the base of Sea of Thieves, Atlas/WaveWorks and Crest. — Background cited via [Sea of Thieves talk notes](https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf); **[bg]** original notes: https://people.computing.clemson.edu/~jtessen/reports/papers_files/coursenotes2004.pdf
- JONSWAP extends Pierson–Moskowitz for fetch-limited seas with a peak-enhancement term γ that sharpens the spectral peak. FFT wave records are always cyclic because the components are harmonically related, which causes visible tiling and is the reason for multiple cascades at incommensurate sizes. — [Modeling Irregular Waves (RWU oceanography text)](https://rwu.pressbooks.pub/oceanhydro/chapter/modeling-irregular-waves/); [LibreTexts spectral analysis](https://geo.libretexts.org/Bookshelves/Oceanography/Coastal_Dynamics_(Bosboom_and_Stive)/03%3A_Ocean_waves/3.04%3A_Short-term_wave_statistics/3.4.3%3A_Spectral_Analysis)
- Horvath, "Empirical Directional Wave Spectra for Computer Graphics" (DigiPro 2015) is the standard graphics reference for JONSWAP/TMA spectra and directional spreading functions in FFT oceans. — [PDF](https://cdn.shopify.com/s/files/1/0134/1131/9867/files/EmpiricalDirectionalWaveSpectra_DigiPro2015_Jun20_2015_Optimized.pdf)
- A 2025 arXiv paper ("Real-Time Interactive Hybrid Ocean: Spectrum-Consistent Wave Particle–FFT Coupling") uses JONSWAP as the global spectrum and couples it with wave particles for interaction. It is recent work on combining FFT with local interaction. — [arXiv 2511.02852](https://arxiv.org/pdf/2511.02852)

**Atlas (GDC 2019, "Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas")**
- Presented by Mark Mihelich (Grapeshot Games) and Tim Tcheblokov (NVIDIA). It covers sea-state simulation, buoyancy physics, sea rendering, and interactive wakes and explosions. The sea uses dual fetch-limited JONSWAP spectra: small/medium local wind waves plus large smooth swell. Atlas was the first game to ship NVIDIA WaveWorks 2.0, which added interactive wakes and explosions. — [GPUOpen slides](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf); [GDC Vault](https://gdcvault.com/play/1025819/Advanced-Graphics-Techniques-Tutorial-Wakes); [NVIDIA blog: WaveWorks 2.0 debuts in Atlas](https://developer.nvidia.com/blog/nvidia-waveworks-2-0-debuts-in-grapeshot-games-atlas)
- WaveWorks runs its simulation in DirectX or Vulkan compute shaders with a CPU fallback. Atlas's server runs a physics-resolution simulation on the CPU and syncs it to clients, which is deterministic, server-authoritative buoyancy. — [NVIDIA blog: three things about WaveWorks 2.0](https://developer.nvidia.com/blog/three-things-you-need-to-know-about-wave-works-2-0); [NVIDIA WaveWorks page](https://developer.nvidia.com/waveworks)

**Sea of Thieves (Rare, SIGGRAPH 2018 Talk "The Technical Art of Sea of Thieves")**
- The ocean is based on Tessendorf FFT. Water colour blends between a deep-water colour and a subsurface colour by view angle, sun direction and a **wave-peak mask** (a cheap subsurface-scattering approximation). There is a GPU surface simulation for splashes on the ship deck, and foam interaction where characters' feet intersect streams and waterfalls. The talk added stylisation on top of a physically based FFT base. — [SIGGRAPH history PDF](https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf); [ACM DOI 10.1145/3214745.3214820](https://unpaywall.org/10.1145%2F3214745.3214820); [MCV dev diary](https://mcvuk.com/development-news/rares-sea-of-thieves-started-with-photorealistic-water/)

**Crest (Wave Harmonic; open source Unity)**
- `wave-harmonic/crest` is MIT-licensed, "a class-leading water system implemented in Unity". It added FFT waves via a `ShapeFFT` component and later doubled FFT sampling from 2 to 4 for quality. Animated FFT waves are the base layer, with a **dynamic wave simulation added on top** for interaction. Crest 5 adds a separate integrated Shallow Water Equations simulation package for large-scale flow (marketed as next-generation; not required for basic shallow water). — [Release notes 4.14](https://crest.readthedocs.io/en/4.14/about/history.html); [Waves docs](https://crest.readthedocs.io/en/4.23.0/user/waves.html); [Crest 5 Asset Store](https://assetstore-fallback.unity.com/packages/tools/particles-effects/crest-water-5-oceans-rivers-lakes-268614); [Crest fork mirror on GitHub](https://github.com/crest-ocean/crest)
- **[bg]** Crest's design (camera-centred geometry LOD clipmap of "LOD data" textures for displacement, dynamic waves, foam, flow and sea-floor depth, all simulated in compute) is described in Huw Bowles' SIGGRAPH 2017 Advances talk "Crest: Novel Ocean Rendering Techniques in an Open Source Framework". It was not fetched this session.

**Heightfield / shallow-water simulation (interactive water, rivers)**
- Müller, Ten Minute Physics #20 "How to write a Water Simulator". Water is an array of columns, each storing height and velocity. Its advantages are easy and fast simulation and easy surface extraction. Its limitations are no overturning waves or splashes. Object coupling uses an extra per-column field for base height. — [Lecture PDF](https://matthias-research.github.io/pages/tenMinutePhysics/20-heightFieldWater.pdf)
- Chentanez & Müller, "Real-time Simulation of Large Bodies of Water with Small Scale Details" (SCA 2010) converts heightfield water to particles for spray and waterfalls and back again. — [PDF](https://matthias-research.github.io/pages/publications/hfFluid.pdf)
- Kass & Miller 1990 is the foundational heightfield approximation: a wave equation with depth-dependent wave speed. — [PDF](https://ohiostate.pressbooks.pub/app/uploads/sites/45/2017/09/kass-miller.pdf)
- NVIDIA Research, "Generalizing Shallow Water Simulations" is recent research on extending shallow-water sims (details not verified). — [NVIDIA PRL page](https://research.nvidia.com/labs/prl/shallow-water-simulation)
- "Ships, Splashes, and Waves on a Vast Ocean" (2021) is academic work coupling ships with large-scale ocean waves. — [ar5iv 2108.05481](https://ar5iv.labs.arxiv.org/html/2108.05481)
- ISTA "Making waves" news (Wojtan group) covers wave-based simulation research for capturing small details in large scenes. — [ISTA: Making waves](https://ista.ac.at/en/news/making-waves/); [ISTA: small details in large scenes](https://ista.ac.at/en/news/new-water-simulation-captures-small-details-even-in-large-scenes/)
- Wave particles (Yuksel, House, Keyser, SIGGRAPH 2007) are a classic cheap interactive wake method. — [SIGGRAPH history](https://history.siggraph.org/?p=115408)

**Water surface shading (Unreal Single Layer Water as an industry reference)**
- UE's Single Layer Water is a physically based shading model with light scattering, absorption, reflection, refraction and water-surface shadowing, using a single depth layer for efficient translucency. Its inputs are Scattering Coefficients, Absorption Coefficients, PhaseG and Color Scale Behind Water, with Blend Mode Opaque/Masked. Refraction reads depth and colour below the surface before distorting samples. — [UE docs: Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine); [UE Water System](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-system-in-unreal-engine)
- UE's Buoyancy plugin includes `FBuoyancyShallowWaterSampler`, which samples water from a baked shallow-water simulation for physics. — [UE Buoyancy API](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/Buoyancy?lang=en-US)

**Buoyancy with Jolt**
- `JPH::Body::ApplyBuoyancyImpulse(inSurfacePosition, inSurfaceNormal, inBuoyancy, inLinearDrag, inAngularDrag, inFluidVelocity, inGravity, inDeltaTime)` treats the fluid surface as a world-space plane with an upward normal. A buoyancy factor of 1 is neutral, <1 sinks and >1 floats. It returns true only if an impulse was applied. The `Body` version does not wake sleeping bodies; use `BodyInterface::ApplyBuoyancyImpulse`, which activates the body only when an impulse is applied. `GetSubmergedVolume` returns total volume, submerged volume and relative centre of buoyancy. A second overload takes precomputed volumes. — [Jolt Body class reference](https://jrouwe.github.io/JoltPhysics/class_body.html); [Body.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/Body.h); [BodyInterface.cpp](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Body/BodyInterface.cpp)

### Inferences
- **Fit to quat-3d, ocean:**
  - Per-cascade spectrum init happens once, or on weather change via the JONSWAP params from the WeatherState wind speed/fetch.
  - Per frame, compute passes do: time evolution, IFFT (Stockham radix-2/4 in shared memory, typically 256² × 3–4 cascades), then assembly of displacement/normal/Jacobian-foam textures with foam persistence.
  - All of this is pure compute in the render graph. Async compute is optional.
  - Wind speed and direction should come from the global weather wind, so ocean, clouds and grass agree.
- **Buoyancy coupling:**
  - Jolt's plane-based `ApplyBuoyancyImpulse` wants one local plane per body (or per sub-shape for long hulls).
  - Recommended: a deterministic CPU evaluation of the ocean at sample points. Either use a low-resolution CPU FFT copy of the largest cascade, or a small sum of Gerstner waves matched to the spectrum's dominant components. This follows the "CPU reference implementation where determinism matters" rule in CLAUDE.md and mirrors Atlas's server-side CPU sim.
  - The alternative is an asynchronous GPU readback a frame late. Fit the plane from 3–4 samples per body, and run inside the fixed 1/120 s step.
  - Rivers can pass `inFluidVelocity` from the flow map.
- **Interactive water / rivers / lakes:**
  - A Müller-style heightfield or shallow-water (SWE) compute sim on a camera- or region-local grid handles ponds, rivers and wakes, layered over the FFT like Crest's dynamic waves.
  - River flow maps can come from the terrain erosion sim's water-flux output, which quat-3d already computes. This is a strong synergy: flow maps drive normal-map advection (two-phase flow-map blending) and foam advection.
  - Jolt bodies inject displacement into the heightfield sim. Wakes and explosions are impulses into the local sim.
- **Shading in a deferred engine:** Render the water surface after the opaque lighting pass as a "single layer". Copy scene colour+depth for refraction and absorption (Beer–Lambert on thickness), apply wave-peak/subsurface approximations (Sea of Thieves), SSR with probe/IBL fallback, foam from Jacobian plus shoreline depth, caustics (projected animated texture or compute caustics from the FFT normal map onto the GBuffer below water) and underwater fog via the froxel volume. This all fits Slang material interfaces.
- **Priority:**
  - Medium: FFT ocean (multi-cascade, JONSWAP) plus single-layer water shading, and Jolt buoyancy via `ApplyBuoyancyImpulse` with CPU sampling. Buoyancy itself is a quick win once any water-height function exists.
  - Medium: heightfield/SWE local sim and erosion-derived river flow maps.
  - Long-term: caustics, wakes/explosions, spray particles (Chentanez–Müller style), underwater rendering, and the hybrid FFT + wave-particle approaches.

### Gaps
- Atlas slide-level details (FFT sizes, cascade counts, wake method, ms cost) could not be read; the gpuopen.com host was blocked.
- Sea of Thieves ms costs and FFT resolution are not confirmed.
- No primary-source performance numbers for FFT ocean or SWE sims on current GPUs were retrieved.
- Crest's internal LOD-data architecture is from background knowledge, not verified this session.
- No primary source found for modern real-time caustics or screen-space refraction in shipped games (2018–2026) during this session.
- Horizon Forbidden West water (Guerrilla) and other 2020s AAA water talks were not retrieved.

---

## 5. Recommended adoption order for quat-3d (synthesis of fit and priority)

### Takeaway
quat-3d's existing atmosphere LUTs, compute grass, GPU terrain/erosion, clustered lighting and Jolt give it unusually good footholds. The highest leverage order is:
1. a global `WeatherState` plus wind field (the shared driver);
2. froxel volumetric fog sharing the clustered grid;
3. a 2.5D Nubis-style cloud layer with temporal reprojection and a cloud shadow map;
4. wetness/snow material layers with a rain occlusion map;
5. FFT ocean plus single-layer water plus Jolt buoyancy;
6. local heightfield/SWE water and erosion-driven rivers;
7. long-term: voxel/SDF clouds and a 3D wind fluid grid.

### Cited Findings
- Frostbite treats sky, atmosphere and clouds as the core trio for dynamic time of day and weather. — [Frostbite 2016 course](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf)
- Hillaire 2020's atmosphere supports runtime composition changes cheaply, so it can be weather-driven. — [EGSR 2020](https://diglib.eg.org/handle/10.1111/cgf14050)
- Froxel fog costs about 1–1.1 ms on PS4-class hardware (AC4; UE4) and uses temporal reprojection with sub-voxel jitter. — [UE docs](https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine); [80.lv (secondary)](https://80.lv/articles/a-cheat-sheet-on-volumetric-fog-in-ue4/)
- 2.5D clouds fit within about 2 ms on PS4. — [Guerrilla Nubis 2017](https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine)
- Ghost of Tsushima-style "volume over accuracy" wind (global direction plus noise) drives grass, cloth and particles at scale. — [Game Developer](https://gamedeveloper.com/design/using-vorticles-to-simulate-wind-in-i-ghost-of-tsushima-i-)
- Jolt provides plane-based buoyancy impulses natively. — [Jolt docs](https://jrouwe.github.io/JoltPhysics/class_body.html)

### Inferences
- Suggested roadmap items:

  | Item | Size | Notes |
  |---|---|---|
  | Weather/wind core | quick win | WeatherState component, Lua/MCP bindings, wind texture to grass |
  | Froxel fog + local fog volumes | medium | depends on R10.1 clustered culling |
  | Volumetric cloud layer + cloud shadows | medium | reuses atmosphere LUTs and TAA history |
  | Rain/snow/wetness | medium | particles, occlusion map, GBuffer-modify pass, deformation RT |
  | FFT ocean + water shading + buoyancy | medium/long | |
  | Local water sim + rivers | long | |
  | Voxel clouds / 3D wind grid | long | |

- Every item can be expressed as render-graph compute passes plus one composite/apply pass, consistent with "GPU compute first". Determinism-sensitive pieces (buoyancy water height, possibly wind for gameplay) should keep a CPU reference implementation tested against the GPU path, per CLAUDE.md.
- Testing (headless GoogleTest): spectrum energy and IFFT round-trip on the CPU reference, buoyancy equilibrium (a floating box settles at the expected draft for a given buoyancy factor), weather-state interpolation, and froxel slice mapping matching the cluster mapping.

### Gaps
- Actual per-pass costs on quat-3d's target GPUs must be measured; published numbers are mostly last-gen console figures.
- The current state of quat-3d's existing "procedural clouds" (2D vs volumetric) was not verified, per the assignment.
