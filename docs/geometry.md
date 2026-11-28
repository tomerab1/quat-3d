# Runtime procedural geometry

`engine_geometry` (ROADMAP R23.1) builds meshes at runtime on the CPU: walls,
buildings, roofs, stones, roads and anything else a game generates instead
of loading. It has no graphics dependency, is part of the SDK's stable API
(`engine/geometry/*.hpp`), and returns `std::expected<…, core::Error>` from
everything that can fail. Degenerate input is refused with a precise error,
never a crash. The design is in
[`design/cozy-builder.md` §4.4](design/cozy-builder.md#44-runtime-procedural-geometry-r23);
the papers are in the [reading list](reading/README.md#procedural-building-curves--geometry).

R23.2 (next) adds the `DynamicMesh` component, its GPU upload path,
colliders, picking, and remote / MCP / Lua access. Until then a game uploads
`MeshData` the way it uploads any mesh.

## Pieces

| Header | What it does |
|---|---|
| `mesh_data.hpp` | `MeshData`: vertices in the GPU layout (`asset::Vertex`), uint32 triangle indices, submeshes per material. `validate`, `compute_bounds`, `mesh_hash`, `append` (with a transform; mirroring keeps faces outward). |
| `mesh_ops.hpp` | `weld`, `compute_normals` (angle-weighted, split at a crease angle), `generate_uvs` (box or planar projection), `compute_tangents` (MikkTSpace), `optimize` (meshoptimizer vertex cache + fetch order). |
| `polygon.hpp` | 2D `Polygon` (outer ring + holes) and `PolygonSet`: `repair`, `offset` (miter / round / square joins), `boolean` (union, intersection, difference, xor), `simplify`, `signed_area`, `contains`. Clipper2 on a 2^-16 m integer grid. |
| `triangulate.hpp` | Polygons with holes to triangles (earcut). Checks that the triangles cover the polygon, so self-intersecting input is reported, not silently mis-filled. |
| `straight_skeleton.hpp` | The weighted straight skeleton of a polygon with holes, and `roof_mesh`: hip, gable (weight 0), shed, mixed-pitch and flat-topped roofs. |
| `sweep.hpp` | `path_frames` (rotation-minimising frames along a polyline), `sweep` (a profile along a path, mitred corners, caps), `loft` (between profiles), `extrude` (a plan polygon upward, with an optional taper). |
| `stone.hpp` | `generate_stone`: seeded rounded-box / icosphere stones with noise and chipped facets. |

Conventions:

- **Plan polygons** are 2D `(x, y)`; outer rings counter-clockwise, holes
  clockwise (`repair` returns that). Generators that lift a plan into 3D put
  it in the world XZ plane: plan `(x, y)` at height `h` becomes `(x, h, y)`.
- **Profiles** for sweeps are 2D in the frame: x sideways (the frame
  normal), y up (the binormal). A counter-clockwise closed profile gives
  outward faces.
- **Units** are metres; UVs are metres / `uv_scale`.

## Roofs

```cpp
using namespace engine::geometry;
Polygon footprint{{{0, 0}, {8, 0}, {8, 4}, {4, 4}, {4, 8}, {0, 8}}, {}};
// One weight per edge: 1 = 45 degree slope, 0 = vertical gable,
// cot(pitch) in general (Kelly & Wonka 2011).
auto skeleton = straight_skeleton(footprint, std::vector<double>{1, 0, 1, 1, 0, 1});
RoofOptions roof;
roof.base_height = 3.0;   // eaves
roof.height_scale = 0.8;  // flatter than 45 degrees
roof.gable_material = 1;  // gable walls in their own submesh
auto mesh = roof_mesh(*skeleton, roof);
```

`max_height` cuts the roof flat at that height (a mansard-like top); the cap
is the union of the cut faces. The skeleton itself (`nodes`, `arcs`,
`faces`, `max_time`) is available for other uses: ridge lines, the medial
"spine" of a footprint, the largest inset before an outline breaks up.

**What is refused.** Self-intersecting rings (run `repair` first), wrong
orientation, weights < 0, a ring with every weight 0, a region that only
weight-0 edges enclose (nothing ever sweeps it), and adjacent parallel
edges with different weights. That last case is ambiguous: the weighted
skeleton has no unique answer there (Biedl et al. 2015). Give parallel edges
that can meet the same weight.

**How it works.** Every edge is a line moving inward at its weight's speed;
wavefront vertices sit on two such lines. Edge events (an edge shrinks to a
point) and split events (a reflex or straight vertex reaches another edge)
are found per vertex and cached, then processed in time order. All events at
one point and time are resolved together as one generalised intersection
event (Kelly & Wonka 2011). This resolves the coincidences that rectilinear
footprints are full of: the four reflex corners of a cross meeting at its
centre, or a strip collapsing onto its ridge. Faces are assembled from the
vertex paths, and the result is checked: the faces of moving edges must tile
the polygon, or the call fails. The fuzz tests run thousands of random stars
(weighted and unweighted), unions of random rectangles (holes, touching
rings) and gable mixes.

## Walls and other sweeps

```cpp
std::vector<glm::dvec3> centre_line = /* sample your curve */;
auto frames = path_frames(centre_line);               // FrameOptions{.closed = true} for loops
std::vector<glm::dvec2> section{{-0.3, 0}, {0.3, 0}, {0.25, 2}, {-0.25, 2}};
SweepOptions sweep_options;
sweep_options.closed_profile = true;
sweep_options.cap_start = sweep_options.cap_end = true;
auto wall = sweep(section, *frames, sweep_options);
```

Corners are mitred, so a wall keeps its thickness through a bend; a corner
sharper than `miter_limit` (default 4x stretch, about 150 degrees) is refused.
Round sharp corners in the path instead. UVs run in metres along the path
(u) and around the profile (v). Paths are polylines for now: G18's
`engine_spline` curves plug in once it lands (TODO in `sweep.hpp`).

## Determinism

Every generator returns bit-identical `MeshData` for the same input on Linux,
Windows and macOS, x86-64 and arm64, so `mesh_hash` can key caches, and
networked or replayed games can regenerate rather than transfer geometry.

- `engine_geometry` compiles without floating-point contraction
  (`-ffp-contract=off`; `/fp:precise` on MSVC). Fused multiply-adds round
  once instead of twice, and compilers fuse differently: GCC fuses across
  statements by default, clang only within one, and arm64 always has FMA.
- No libm transcendentals: `sin`, `cos`, `atan2` and `acos` come from
  `src/geometry/det_math.cpp` (fdlibm kernels, only `+ - * /` and `sqrt`),
  because the C runtimes' versions differ in the last bit.
- MikkTSpace is vendored (`src/geometry/vendor/mikktspace`, zlib licence)
  rather than taken from vcpkg, so it builds under these rules; its `cos` and
  `acos` calls go to `det_math`.
- Random numbers are SplitMix64 (integers only); noise is integer-hashed.
- `tests/geometry/golden_test.cpp` pins hashes of roofs, a swept wall, stones
  and a tapered extrusion. They were computed with clang on Linux and also
  match GCC 13 at `-O3 -march=x86-64-v3`; CI checks MSVC and Apple clang.

**The exception is Clipper2**, which vcpkg builds with each platform's default
floating-point settings. Its integer arithmetic is exact, but intersection
points and round joins are computed in doubles and then rounded to the grid,
so a result can differ by one grid step (15 µm) in a rare half-way case.
Polygon operations, `repair`, flat-topped roofs and outward tapers use
Clipper2. If bit-exactness matters there, snap the output to a coarser grid
before hashing it.

## Cost

The straight skeleton caches events, so its cost grows roughly
quadratically: a 100-vertex footprint takes about 1 ms and 1000 vertices
about 70 ms (one core, release build). Sweeps, extrusions and stones are
linear in their output.
