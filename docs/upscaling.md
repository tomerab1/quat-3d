# Velocity, jitter and upscaling (R11)

How a frame gets from jittered render-resolution samples to an anti-aliased
display image, and the contract every temporal technique (TAA, upscalers,
later motion blur and reprojected effects) builds on. Code is the source of
truth for details; this page explains the shape and the FSR plan.

## Frame structure

```
render extent  : shadow → GBuffer (mesh, terrain, grass) → background velocity
                 → light culling → lighting → transparent
upscaler       : render extent → display extent (pre-tonemap HDR)
display extent : bloom → exposure → tonemap → present copy → editor UI
```

`src/app/frame_graph.cpp` builds this. The render extent is the display (draw)
extent scaled by the upscaler settings (`upscaler_settings.hpp`): `native_aa`
1.0, `quality` 1/1.5, `balanced` 1/1.7, `performance` 1/2 per axis, or an
explicit `render_scale` override. Material textures are sampled with a LOD
bias of log2(render / display) (`texture_lod_bias`), so upscaled frames keep
display-resolution texture detail.

## Jitter contract (`engine/renderer/jitter.hpp`)

One source: `halton_jitter(frame, render_extent, phase_count)` gives a
Halton(2, 3) offset in render pixels and NDC. The cycle is 8 phases at native
resolution, times the pixel-area ratio when upscaling (AMD's guidance), so
every display pixel receives samples within one cycle. The frame rasterises
with `jitter_view_proj(view_proj, jitter.ndc)`; render pixel `i` therefore
shows the unjittered UV `(i + 0.5 - jitter.pixels) / render_extent`. Every
consumer (velocity writers, the resolve, future backends) takes the jitter
from the same `ViewJitter` value.

## Velocity contract (`engine/renderer/velocity.hpp`)

An RG16F target at the render extent holds, for every pixel,
`uv_current - uv_previous` of the surface it shows, both computed with
UNjittered projections (the previous position is `uv - v`; a static surface
under a static camera stores exactly 0). The shader side is
`assets/shaders/lib/velocity.slang`.

| Writer | Previous-frame data |
|---|---|
| Meshes (`MeshPass`) | previous model matrix per instance (`InstanceMotion`, from `MotionHistory`, keyed by entity) |
| Skinned meshes | `SkinningPass` also skins with last frame's joint palette and writes previous positions (float4 per vertex) |
| `InstancedMesh` placements | the entity's previous world transform times each placement |
| Grass | each blade evaluated again at the previous wind time |
| Terrain | static: camera motion only |
| Background | `VelocityPass`: far-plane camera motion where depth is still 1 |

Fallback: anything without previous data (new entity, a draw list built
without motion records, a camera cut) uses its current transform as the
previous one and gets camera-only velocity. Transparent and particle passes do
not write velocity; the upscaler interface has a reactive-mask input reserved
for them.

## Upscaler interface (`engine/renderer/upscaler.hpp`)

`Upscaler::add_to_graph(graph, slot, inputs)` takes pre-tonemap linear HDR
colour, depth, velocity, the exposure buffer (adapted luminance from the
previous frame), an optional reactive mask, the jitter, both extents,
sharpness and a reset flag, and returns an RGBA16F display-extent image.
`create_upscaler(backend, …)` builds a backend; one that is not available
returns an error and the frame loop keeps TAAU.

### TAAU (`taa_pass.hpp`, `assets/shaders/taa.slang`) — the default backend

Per display pixel: reconstruct the current frame from the 3×3 nearest render
samples (Gaussian in display pixels; the nearest sample's kernel weight is the
frame's confidence for that pixel), reproject the history with the velocity of
the closest-depth sample (depth dilation), sample it with a 5-tap Catmull-Rom
filter, clip it toward the current neighbourhood (variance clipping inside the
min/max box) and blend with weight `confidence / accumulated` — a running
average over the samples that landed on the pixel, capped at 10, whose weight
drops when clipping moves the history. All of it in exposed,
Reinhard-compressed YCoCg. The history is RGBA32F: half floats stall under
the small per-sample weights of the average (measured: a 50% upscale's error
rose from 0.0017 to 0.0047). At native resolution this is TAA.

Verified by `tests/render/taa_test.cpp` (history across frame slots, exact
static reprojection, reset on resize, clamping, ghosting of a moving object
with and without velocity) and `tests/render/upscaler_test.cpp` (50% render
scale against a supersampled reference, native TAA and a bilinear upscale).

## FSR 2 / 3.1 backend — not built yet

AMD's FidelityFX FSR 2 (MIT) is the planned second backend
(`UpscalerBackend::fsr2`). It is not integrated because neither official
distribution builds in this project as shipped:

- The FidelityFX-FSR2 repository (v2.2.x) and FidelityFX SDK 1.x build only
  on Windows: their CMake rejects non-x64/Win32 platforms and compile the
  shader permutations with the Windows-only `FidelityFX_SC.exe`. There is no
  vcpkg port.
- Their Vulkan backend allocates memory with `vkAllocateMemory`, uses
  descriptor pools / `vkAllocateDescriptorSets` and legacy
  `vkCmdPipelineBarrier` — all excluded by CLAUDE.md, and invisible to the
  render graph's barrier planning.

The integration path that keeps the engine's model intact:

1. Vendor the platform-independent FSR 2 core (`ffx_fsr2.cpp`, its headers
   and the GLSL pass sources under `src/ffx-fsr2-api/shaders`), pinned to a
   release tag, MIT licence kept.
2. Implement FSR's backend callback table (`FfxFsr2Interface`: create
   resource / pipeline, schedule GPU job, execute jobs) on the engine RHI:
   VMA allocations, descriptor buffers with reflected bindings, and each FSR
   dispatch recorded as a render-graph compute pass so sync2 barriers come
   from the graph.
3. Cook FSR's passes offline with the engine's shader pipeline (glslang for
   the GLSL sources, or a Slang port), so macOS (MoltenVK) uses the same
   compute-only path.
4. Map the inputs: our velocity is `uv_current - uv_previous`; FSR reprojects
   with `previous = current + motion` in render pixels, so pass
   `motionVectorScale = -render_extent`. Pass `jitter.pixels` with the sign
   FSR's documentation gives for its projection convention (check it with
   FSR's debug view: a static scene must not shimmer). Exposure comes from
   the same adapted-luminance buffer (as a 1×1 texture).

DLSS, XeSS and FSR 4 ship as binaries and belong behind the same interface as
runtime-loaded plugins (ROADMAP R11 follow-ups).
