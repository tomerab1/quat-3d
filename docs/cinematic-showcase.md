# Cinematic showcase: a forest clearing at golden hour

`assets/scenes/cinematic_forest.json` is a pine-forest clearing at golden
hour built only from real downloaded assets: CC0 models, a ground texture
and an HDRI from [Poly Haven](https://polyhaven.com). Nothing large is
committed: the scene references files a script downloads, verifies and cooks
into gitignored caches. This page lists the assets, shows how to fetch and
render the scene, and gives the MCP recipe a model can follow to build
scenes like it.

## Fetch the assets

```sh
cmake --build --preset release --target game quat-cook
python3 scripts/fetch-polyhaven.py assets/showcase/cinematic/assets.json \
  --cook-with build/release/quat-cook
```

About 560 MB is downloaded (480 MB of it the full fir tree's geometry) into
`assets/meshes/polyhaven/`, `assets/textures/polyhaven/` and
`assets/hdri/polyhaven/`, exactly where quat-mcp's `asset_fetch` puts
models. Each file is checked against the size and MD5 Poly Haven publishes;
re-running only verifies. The cooker then writes one GLB per model (KTX2
textures, meshoptimizer meshes); the heavy firs are cooked to two levels of
detail with `quat-cook --simplify` (6 % for the rows around the clearing,
1.5 % for the rows behind), the saplings to 25 %. The first cook takes a few
minutes (most of it the fir). CI does the same before its screenshots and
caches the result.

## Assets (all CC0, attribution recorded)

| Poly Haven id | Kind | Res. | Use in the scene | Authors |
|---|---|---|---|---|
| [`fir_tree_01`](https://polyhaven.com/a/fir_tree_01) | model | 1k | the forest (3 variants, 18 m), cooked to 6 % (near rows) and 1.5 % (far rows) by thinning its needle cards, BLEND twigs made alpha-tested | Rob Tuytel (photography), Rico Cilliers (modeling) |
| [`fir_sapling`](https://polyhaven.com/a/fir_sapling) | model | 1k | young firs in the clearing (cooked to 25 %) | Rob Tuytel, Rico Cilliers |
| [`pine_sapling_small`](https://polyhaven.com/a/pine_sapling_small) | model | 1k | young pines in the clearing (cooked to 25 %) | Rob Tuytel, Rico Cilliers |
| [`fern_02`](https://polyhaven.com/a/fern_02) | model | 1k | foreground framing and ground cover (4 variants) | Rob Tuytel (scanning), Rico Cilliers (modeling) |
| [`dead_tree_trunk`](https://polyhaven.com/a/dead_tree_trunk) | model | 1k | the hero fallen log | Rob Tuytel |
| [`tree_stump_01`](https://polyhaven.com/a/tree_stump_01), [`tree_stump_02`](https://polyhaven.com/a/tree_stump_02) | model | 1k | stumps | Rob Tuytel |
| [`rock_moss_set_01`](https://polyhaven.com/a/rock_moss_set_01), [`rock_moss_set_02`](https://polyhaven.com/a/rock_moss_set_02) | model | 1k | mossy rocks and boulders | Rob Tuytel |
| [`dry_branches_medium_01`](https://polyhaven.com/a/dry_branches_medium_01) | model | 1k | fallen branches | Rico Cilliers |
| [`forest_leaves_04`](https://polyhaven.com/a/forest_leaves_04) | texture | 1k | forest-floor ground plane (generated `ground.gltf`) | see `polyhaven.json` |
| [`spaichingen_hill`](https://polyhaven.com/a/spaichingen_hill) | HDRI | 2k | sky / image-based light once HDRI environments land; the sun is matched to it | see `polyhaven.json` |

The manifest, `assets/showcase/cinematic/assets.json`, is the source of
truth: id, resolution, licence, authors, what each asset is for and how it
is cooked. Every download folder also gets a `LICENSE.md` and a
`polyhaven.json` (source URL and checksum of every file).

## The scene

- **Layout.** A clearing about 25 m across, ringed by full-size firs: near
  rows (6 % LOD) 16–30 m from its centre, far rows (1.5 % LOD) out to
  about 70 m, one `scatter` per variant and belt. Young firs and
  pines, ferns and mossy rocks are scattered inside; the hero fallen log,
  a stump, two rocks and two branches are placed by hand in the middle
  ground, each seated on the ground by the lowest point of its mesh (from
  the glTF's bounds) and sunk a few centimetres so it looks embedded; four
  ferns sit right in front of the lens as foreground framing.
- **Variety.** Every scatter mirrors half its placements (`scatter.mirror`,
  added for this scene) and varies yaw and scale; each model variant (a
  glTF node such as `fern_02_c`) is its own scatter with its own seed.
- **Light.** One sun 13° above the horizon, warm (`[1.0, 0.78, 0.55]`),
  matching the HDRI's sun elevation and colour, placed 35° to the left of
  the hero camera's axis: the scene is back-lit, so long shadows run towards
  the lens and the grass and fern edges catch the light. Fill comes from the
  sky (the dynamic IBL probe of the procedural atmosphere).
- **Ground.** The forest-floor texture on a generated 400 m plane, under a
  short (2.5–13 cm), dense (320 blades/m²) GPU grass field with golden tips (`grass`, `terrain_only: false`) with a light wind.
- **Camera.** `HeroCamera`: 0.42 m above the ground, 38° vertical field of
  view (about a 35 mm lens), the log on the left third, the bright gap in
  the tree line on the right third.
- **Render settings.** Auto exposure, soft bloom, GTAO, contact shadows and
  long-reaching sun shadows (`render_settings` in the scene file).

### Features it is ready for

Scene files are read leniently: a key this build does not know is skipped
with a log line, so the scene loads on builds with or without these:

- **Weather** (R13, PR #64): a `Weather` entity sets a light breeze along
  the grass wind (`wind_speed` 3 m/s) and a few clouds.
- **HDRI sky + IBL** (lighting realism PR): point the environment at
  `assets/hdri/polyhaven/spaichingen_hill/spaichingen_hill_2k.hdr`; rotate
  it so its sun (36° right of the image centre, 12.9° up) lines up with the
  scene's sun.
- **Physical camera, depth of field, grading** (R20, cinematic post PR):
  the close-up shot wants a shallow depth of field focused on the log
  (about 0.9 m away, f/2), and an AgX tonemap with a warm grade.

### Cost

On lavapipe (CPU Vulkan) a 1920x1080 frame of the hero view takes about
18 s on four loaded cores; most of it is the forest (about 6 M triangles
after the LOD cook). Engines ship forests with runtime LOD selection and
impostors for the far rows; quat-3d has neither yet, so the cook-time LODs
and the row densities are the budget.

## Rendering

The CI screenshot job renders the hero camera (`cinematic` row of
`scripts/ci-screenshots.sh`, Play mode so the scene camera is the view).
The stills come from `scripts/render-shots.py`, which drives the engine over
the same JSON-RPC endpoint quat-mcp uses (`camera.set`, `render.screenshot`):

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  scripts/render-shots.py assets/showcase/cinematic/shots.json /tmp/cinematic
```

`shots.json` lists the shots (hero, wide establishing, medium, close-up) as
camera position / target / field of view, plus optional per-shot calls
(render settings, sun) sent before the shot.

## MCP recipe

How a model builds this kind of scene with the quat-mcp tools. Every call
below is a real tool with its real parameters; the scene file is what
`scene_save` writes at the end.

1. **Find and fetch assets.** `asset_search {"query": "fern"}`, `{"query":
   "rock moss"}`, `{"query": "fir", "category": "trees"}`; prefer assets of
   one Poly Haven collection (here `pine_forest`) so they match. Check
   `polycount`: thousands of instances need a few thousand triangles each.
   `asset_fetch {"id": "fern_02"}` (1k) for each; its `alpha_modes` tells you
   whether the foliage is MASK or BLEND. Heavy or BLEND foliage is cooked
   with `quat-cook --simplify 0.05 --blend-to-mask 0.5` (the manifest's
   `cook` entries record this).
2. **Ground and grass.** One `scene_batch` with `scene.create` commands:
   the ground model as an `instanced_mesh` with one instance, then
   `{"name": "ClearingGrass", "components": {"grass": {"density": 70,
   "radius": 30, "blade_height": [0.12, 0.42], "terrain_only": false,
   "ground_y": 0, "wind_strength": 0.25}}}`.
3. **Hero props by hand.** `instanced_mesh` with explicit `instances`
   (`position`, `rotation` in degrees, `scale`) for the log, stump and the
   rocks the eye should land on; pick one variant with `node`
   (`"node": "rock_moss_set_02_rock13"`). Seat each one with
   `world_place_on_ground` (it reads the model's bounds) instead of guessing
   a height, then sink logs and rocks 2–10 cm.
4. **Scatter the rest.** One entity per variant:
   `{"transform": {"position": [0, 0, -6]}, "instanced_mesh": {"gltf":
   "assets/meshes/polyhaven/fern_02/fern_02.glb", "node": "fern_02_a"},
   "scatter": {"seed": 21, "size": [44, 34], "density": 0.12,
   "min_spacing": 0.9, "scale": [0.8, 1.5], "mirror": 0.5}}`. Big trees go
   in belts around the open area (`size` rectangles), not over it.
5. **Light.** `environment_set {"sun": {"elevation_deg": 13,
   "azimuth_deg": <35° off the camera axis, towards the camera>,
   "color": [1.0, 0.78, 0.55], "intensity": 6}, "clouds": {"coverage":
   0.3}}`: low and in front of the camera reads as golden hour.
6. **Camera.** A `camera` entity low to the ground (0.3–0.5 m) for the
   hero shot, the subject on a third line, foreground elements at the frame
   edges; check with `screenshot {"camera": {"position": [...], "look_at":
   [...], "fov_deg": 38}}` (A9) or `camera_set` + `screenshot`.
7. **Look.** `render_settings_set` one group at a time and compare
   screenshots: `{"shadows": {"max_distance": 80, "sun_size_deg": 0.8}}`,
   `{"ao": {"radius": 0.8, "strength": 1.5}}`, `{"bloom": {"intensity":
   0.08}}`, `{"exposure": {"compensation_ev": 0}}`.
8. **Save.** `scene_save {"path": "assets/scenes/<name>.json"}`.
