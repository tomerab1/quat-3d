# Volumetrics: clouds, aerial perspective and fog

ROADMAP **R13.4** (volumetric clouds v2) and **R13.3** (aerial perspective,
froxel fog, fog volumes). Everything here runs on the GPU as compute passes
in the render graph, between the lighting pass and the transparents:

```
lighting ──▶ aerial_perspective ──▶ cloud_march ──▶ cloud_temporal ──┐
             (32x32x32 froxels)     (¼ resolution)  (reprojection)   │
         ──▶ fog_scatter ──▶ fog_integrate ─────────────────────────┤
             (160x90x64 froxels, temporal)                          ▼
                                                              atmospherics (composite)
```

`VolumetricsRuntime` (`src/app/volumetrics_runtime.cpp`) builds the frame's
inputs (settings, weather, world clock, fog volumes) and declares the passes;
`FrameRenderer::add_volumetrics` (`src/app/frame_volumetrics.cpp`) is the
one call in the frame graph.

## Clouds v2

The model is Schneider's **Nubis** (Horizon Zero Dawn, SIGGRAPH 2015 / 2017)
lit the **Frostbite** way (Hillaire, SIGGRAPH 2016):

- **Noise** (`lib/cloud_noise.slang`), baked once by compute at startup
  (`CloudNoisePass`): a 128³ base volume (Perlin-Worley in r, Worley fBm in
  gba), a 32³ detail volume (Worley fBm), a 128² curl-noise texture and a
  512² weather-noise texture. The lattices use an integer hash (pcg3d), so
  the CPU mirror (`src/renderer/cloud_noise.cpp`) bakes the same texels;
  tests compare them.
- **Shape** (`lib/clouds.slang`): coverage, cloud type (stratus,
  stratocumulus, cumulus) and precipitation come from the weather
  (`weather.cloud_cover`, `weather.precipitation`) and the `clouds` render
  settings. The weather noise varies them over the world. Height profiles
  per type; coverage carves the base shape; curl-distorted detail erodes the
  edges (wisps at the base, billows at the top). A closing sky turns toward
  layered cloud; rain clouds are thicker. A thin **cirrus** layer (single
  scattering of a slab) sits at `cirrus_altitude_km`.
- **Light**: energy-conserving integration of each step, a dual-lobe
  Henyey-Greenstein phase function, multiple scattering by Wrenninge's
  octaves, the sky's ambient light. The **sun and the moon** (R22.1
  `SkyState`, when the scene has a world clock) each light the clouds
  through the atmosphere's transmittance LUT with their own irradiance, so
  sunsets redden and a full moon lights the clouds at night.
- **Rendering**: the march runs at a quarter of the pixels (half width x
  half height) with adaptive steps (coarse through empty space, fine in a
  cloud) and a per-frame jittered start; the temporal pass reprojects last
  frame's result through the camera motion and the wind drift and blends,
  so a still view converges to a clean image. The composite upsamples it
  over the lit image (the sun's disc is dimmed by the real transmittance)
  with the aerial perspective in front of the clouds.
- **Shadows**: with a weather in the scene, the cloud shadow map (R13.1)
  integrates the same clouds, so shadows sit under the visible clouds.
- **Reflections and ambient**: the dynamic IBL probe bakes the same layer
  (per texel, with the aerial perspective from the transmittance LUT), and
  refreshes when the layer changes or has drifted 150 m.

### Settings (`clouds` group)

| Field | Default | Meaning |
|---|---|---|
| `enabled` | true | Draw the clouds |
| `coverage` | 0.42 | Covered fraction (the weather's `cloud_cover` replaces it) |
| `density` | 24 /km | Extinction at full density |
| `bottom_km`, `thickness_km` | 1.5, 2.5 | The cloud layer |
| `size_km` | 10 | Base feature size |
| `detail` | 0.4 | Edge erosion |
| `type` | 0.8 | 0 stratus, 0.5 stratocumulus, 1 cumulus |
| `cirrus_coverage`, `cirrus_altitude_km`, `cirrus_opacity` | 0.35, 9, 0.35 | Cirrus layer |
| `march_steps` | 48 | Samples of a vertical ray (twice as many at the horizon) |
| `wind_speed`, `wind_dir_deg` | 12, 35 | Drift without a weather (the weather's wind drives it otherwise) |
| `shadows` | true | Cloud shadows (with a weather) |

`QUAT_WEATHER=<clear | cumulus | overcast | rain | valley_fog>` starts the
game with a weather preset (the CI screenshot rows use it).

## Aerial perspective

Hillaire 2020's camera-frustum volume (`atmos_aerial.slang`): in-scattering
and transmittance of the atmosphere integrated along 32x32 view rays into 32
slices up to 96 km (quadratic spacing), for the sun and the moon. Opaque
pixels take the haze of the air in front of them at their distance; the
clouds at their transmittance-weighted depth. The sky already holds its own.

## Volumetric fog

**Froxel fog** (Wronski, SIGGRAPH 2014; Hillaire, SIGGRAPH 2015): a
160x90x64 camera-aligned grid, slices spaced logarithmically to
`fog.max_distance_m`.

- **Media**: an exponential height fog (`fog.density` /m below
  `fog.base_height_m`, thinning by `fog.height_falloff` /m above) plus every
  `FogVolume` entity — a box or ellipsoid placed by its Transform (the unit
  volume scaled into the world) with its own extinction, albedo, soft edges
  and height falloff.
- **Light**: the directional light through the cascaded shadow map and the
  cloud shadow map (light shafts), the clustered point and spot lights of
  R10.1 (each froxel reads its cluster's list), the sky's ambient (the IBL
  irradiance), with a Henyey-Greenstein phase (`fog.anisotropy`).
- **Filtering**: the sample position is jittered per frame inside each
  froxel and the result reprojected and blended with the previous frame.
- **Integration** front to back with the energy-conserving form; beyond the
  grid's range the same height fog continues in closed form, so distant
  terrain and the horizon fog without a seam.

Extinction is physical: meteorological visibility is about 3.9 / extinction
(0.004 /m ≈ 1 km visibility).

### Settings (`fog` group)

| Field | Default | Meaning |
|---|---|---|
| `enabled` | true | Volumetric fog (volumes + height fog) |
| `density` | 0 /m | Height fog extinction at its base (0 = none) |
| `height_falloff` | 0.02 /m | How fast it thins above the base |
| `base_height_m` | 0 | World height below which it is uniform |
| `albedo` | 0.9 | Scattering / extinction |
| `anisotropy` | 0.6 | Forward scattering |
| `max_distance_m` | 1000 | Range of the froxel grid |

## Performance

Measured on **lavapipe** (Mesa's CPU Vulkan driver, 4 vCPU Xeon @ 2.3 GHz) at
a **1920x1080** render extent, one frame, submission included (median of 4):

| Pass | lavapipe | Expected on a current GPU (approximate) | Basis of the estimate (figures reported in the talks, rounded) |
|---|---|---|---|
| Aerial perspective (32³) | ~5 ms (mostly fixed overhead) | ~0.02-0.05 ms | Hillaire 2020: 0.04 ms (AP LUT) |
| Clouds: march + temporal (960x540) | ~26 ms | ~0.8-1.5 ms | Schneider 2015: 2 ms on PS4 (1/16 res/frame); Hillaire 2016: ~1-2 ms on console at ¼ res |
| Froxel fog: scatter + integrate (160x90x64) | ~16 ms | ~0.3-0.6 ms | Wronski 2014: ~1.1 ms on Xbox One (160x90x64); Hillaire 2015 similar |
| Composite (full res) | ~11 ms | ~0.1-0.2 ms | 6 texture fetches per pixel |
| Noise bake (startup, once) | ~1 s | a few ms | |

Lavapipe runs these compute passes 50-200x slower than a desktop GPU; the
ratios between passes are the useful part. The cloud march scales with
`march_steps` and the covered fraction (empty space is skipped on the cheap
density); the fog with the number of fog volumes and clustered lights per
froxel.

## Tests

- `tests/renderer/cloud_noise_test.cpp`, `cloud_reference_test.cpp` (CPU):
  noise tiling and ranges, GPU bake = CPU bake, exact step integration,
  normalised phase functions, octave energy bounds, a lit slab against its
  closed form, the layer the frame builds.
- `tests/render/volumetric_clouds_test.cpp`: clouds appear where coverage
  puts them, a static camera converges, bit-identical runs (fixed dt),
  coverage / overcast ordering.
- `tests/render/volumetric_fog_test.cpp`: a uniform medium against
  `B e^{-σd} + ω A (1 - e^{-σd})`, no seam at the grid's range, local
  volumes, stability and determinism.
- `tests/render/cloud_shadow_test.cpp`: the cloud shadow map against the
  CPU reference.

Set `QUAT_TEST_DUMP_DIR=<dir>` to write the tests' images as PPM.

## Limitations (follow-ups)

- Transparents and particles are drawn after the composite, so they are
  not fogged yet.
- `FogVolume` has no scene-file codec / editor / remote yet (it lands with
  S9's per-module component registration).
- Clouds are a 2.5D layer seen from below; flying through them works
  (density is volumetric) but the march starts at the camera height of the
  sky model (200 m).
