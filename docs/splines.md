# Splines, curve networks and stroke capture

ROADMAP G18 (design: [`design/cozy-builder.md` §4.3](design/cozy-builder.md#43-splines-and-curve-networks-g18)).
`engine_spline` is a CPU-only, Vulkan-free library. Its headers are part of
the SDK's stable API:

| Header | What it gives you |
|---|---|
| `engine/spline/spline.hpp` | `Spline` data, kinds, validation, the cubic Bézier segments every query uses |
| `engine/spline/curve.hpp` | `Curve`: evaluation by parameter or distance, arc length, bounds, `sample_by_spacing` |
| `engine/spline/frames.hpp` | Up-locked and rotation-minimising frames |
| `engine/spline/queries.hpp` | Closest point, XZ crossings, self-crossings, flattening, offsets |
| `engine/spline/ground.hpp` | Draping samples and points on the ground (`GroundHeightFn`) |
| `engine/spline/network.hpp` | Curve networks: junction graphs with stable ids |
| `engine/spline/stroke.hpp` | Stroke capture: pointer drag to spline |
| `engine/scene/spline.hpp` | The `spline` component, its `SplineCache` and `build_curve_network` |

Games, the editor, Lua and the remote API all use the same code.

## The `spline` component

```json
"spline": {"kind": "centripetal_catmull_rom", "closed": false,
           "follow_ground": true, "ground_offset": 0, "network": 0, "id": 0,
           "points": [[0, 0, 0], {"position": [6, 0, 2], "width": 2, "height": 3, "tags": 1}, [12, 0, 0]]}
```

- **Kinds.** The default is `centripetal_catmull_rom`. It passes through
  every point, and it has no cusps or self-intersections within a segment
  (Yuksel, Schaefer & Keyser 2011), so player-drawn curves never loop.
  `cubic_bezier` takes points as anchor, handle, handle, anchor, …
  (3k + 1 points open, 3k closed). `polyline` draws straight segments.
- **Per-point attributes.** `width` is the extent across the curve, for
  example a path's width or a wall's thickness. `height` is the extent
  above it, for example a wall's height. `tags` are game-defined bits.
  Width and height interpolate linearly along each segment. A sample's
  tags are those of its segment's start point.
- **Coordinates.** Points are entity-local, so moving the entity moves the
  curve.
- **Ground.** `follow_ground` drapes the cached samples on the terrain,
  `ground_offset` metres above it. The points keep their own heights.
- **Network.** `network` (non-zero) joins splines into a curve network.
  `id` is the spline's stable id inside it; 0 means the entity's id.

A point with the defaults (width 1, height 0, tags 0) is written as a bare
`[x, y, z]`. The codec validates the whole spline after a merge: an edit
that makes it impossible, such as closing a two-point curve, fails with the
field at fault (`spline.points: a closed centripetal_catmull_rom spline
needs at least 3 points, got 2`). A spline with no points at all is a valid
component, as when it is added empty and then drawn. Its cache reports why
it has no curve yet.

### SplineCache

`update_spline_caches` runs every frame after the scene tick. It keeps a
`SplineCache` next to each `Spline`, holding the world-space `Curve`,
samples every 0.5 m (both ends included, draped when the spline follows
the ground) and bounds swept by width and height. A cache is rebuilt only
when the spline, its world matrix or the spacing changed. Each rebuild bumps
its `revision`. Samples over terrain that has not streamed in yet are
counted in `missing_ground` and retried every 30 frames. An invalid spline
keeps a cache with `valid = false` and the reason in `error`.

## Curves

`Curve::build(spline)` compiles a spline into cubic Bézier segments. The
parameter `t` runs over `[0, segment_count]`, and the distance `s` over
`[0, length()]`.

- **Arc length.** Each segment is split adaptively until a 5-point
  Gauss–Legendre rule agrees with itself on both halves, and the
  cumulative lengths are tabled. `param_at(s)` inverts the table by binary
  search and then by Newton steps with a bisection fallback.
- **Even spacing.** `sample_by_spacing(curve, {spacing, phase,
  fit_to_ends})` returns samples, each with position, unit tangent, width,
  height, tags, parameter and distance. On a closed curve the samples make
  one lap from the phase. `fit_to_ends` adjusts the spacing so samples land
  on both ends, as fence posts should.
- **Bounds** are exact, from the roots of each segment's derivative.
  `swept_bounds()` grows them by half the width and by the height.

### Frames

`frames(samples, mode)` and `sample_frames(curve, spacing, mode)` return
`{position, tangent, up, right}`, with `right = cross(tangent, up)` pointing
to the right of travel. `Frame::matrix()` maps +X to the tangent, +Y to up
and +Z to right.

- `up_locked` keeps up at world +Y. Use it for walls and fences, which stay
  vertical on slopes.
- `rotation_minimising` uses the double-reflection method (Wang, Jüttler,
  Zheng & Liu 2008). It twists as little as possible and never flips (the
  tests check a helix), for rails, ribbons and ivy. Pass dense samples,
  because each frame is propagated from the previous one.

### Queries

- `closest_point(curve, p, xz_only)` returns the parameter, distance along
  the curve, position and gap. It prunes segments by bounding box, then
  refines by Newton steps on (B(u) − p)·B′(u).
- `crossings_xz(a, b)` and `self_crossings_xz(c)` find crossings seen from
  above. Both curves are flattened to chords, the chords are tested through
  a bounding-volume hierarchy, and each hit is refined on the curves by
  Newton steps.
- `offset_xz(curve, {distance, width_factor, spacing})` returns a parallel
  polyline. `width_factor = ±1` traces the edges of a path of the curve's
  widths. Parts closer to the curve than the offset distance are cut away:
  the swallowtails inside tight bends and a U-turn's overlapping inner side
  (the distance test of Seong, Elber & Kim 2006).
- `flatten(curve, tolerance)` returns an adaptive polyline.
- `drape(samples, ground, offset)` and `project_points(spline, ground,
  offset)` put samples or points on the ground. `GroundHeightFn` is the
  same type as `scene::GroundHeightFn`.

### Determinism

The library uses only + − × ÷ and √, in a fixed order, and
`engine_spline` is compiled without floating-point contraction (no FMA
fusion). The same input therefore gives bit-identical output on Linux,
Windows and macOS (arm64). `SplineDeterminism.SampleBitsMatchTheReference`
pins a hash of sample bits and runs on every CI platform, and
`tests/spline/determinism_test.cpp` does the same for every kind,
`transformed()`, both frame modes, stroke fits in both outputs, a network
with a T and an X, and trimmed offsets. If you change an algorithm on
purpose, update the reference hash in the same commit.

The guarantee covers the library's inputs. The scene layer (`SplineCache`,
the remote methods) applies the entity's world matrix, which `engine_scene`
composes with the platform's default contraction. A deep hierarchy's matrix
can therefore differ by an ulp between platforms before it reaches the
library. Keep spline entities at the root, or with exactly representable
transforms, where bit-identical output matters.

**Known limitations**

- On a closed, non-planar curve, rotation-minimising frames have a twist
  step at the seam (no holonomy correction yet).
- Offsets test each sample against its own distance, which approximates
  the envelope when widths vary.
- A network's crossing ids follow the crossing's index along its pair.

## Curve networks (G18.2)

`build_network(curves, {snap_radius, straight_cos})` builds a planar graph
from many curves, seen from above. In a scene, call
`scene::build_curve_network(registry, network)`; remotely, call
`spline.network`.

1. Endpoints within `snap_radius` (0.25 m) of each other join one node.
2. An endpoint within the radius of another curve's interior splits that
   curve there, making a T.
3. Curves split where they cross each other or themselves. Crossings near an
   existing node join it, so three curves through one point make one node.
4. A closed curve that meets nothing gets one node at its start.

Each node has a `kind`, chosen from its degree and angle:

| Kind | Meaning |
|---|---|
| `end` | Degree 1 |
| `straight` | Degree 2, with the edges within 15° of opposite |
| `bend` | Degree 2 at any other angle |
| `tee` | Degree 3 |
| `cross` | Degree 4 |
| `n_way` | Degree 5 or more |

A node's `ends` list the edges that leave it, counter-clockwise seen from
above starting at +X, each with its XZ direction. Each edge records its
source curve, its parameter range `t0..t1` on that curve and its length. On
a closed curve, `t1` can exceed the curve's maximum parameter, meaning the
edge runs over the seam (`Curve` wraps the parameter).

**Ids.** Node and edge ids are 64-bit hashes of what they join:

- An endpoint node is named after its lowest (curve id, end) member.
- A crossing node is named after the two curve ids and the crossing's index
  along the pair.
- An edge is named after its curve and its two nodes.

Reshaping a curve without changing what it meets therefore keeps every id.
Adding a curve that crosses one edge changes only that edge. A new crossing
between the same two curves, earlier along their pair, renumbers the later
crossings of that pair: carrying ids over from a previous network is a
planned follow-up. The result does not depend on input order. Remote
replies write ids as 16 hex digits.

## Stroke capture (G18.3)

`fit_stroke(points, options)` turns ground hits into a clean spline in
these steps:

1. Resample the hits by distance (`sample_spacing`).
2. Remove hand jitter with Ramer–Douglas–Peucker (`jitter_tolerance`).
3. Split at corners sharper than `corner_cos`.
4. Fit each piece with Schneider's least-squares cubics (Graphics Gems
   1990), within `fit_tolerance`.
5. Convert to centripetal Catmull-Rom points. Starting from the fit's
   anchors, a span's midpoint is added wherever the curve leaves the fit
   by more than the tolerance; both directions are checked, so overshoot
   at corners is caught. Sharp joints (corners and both ends of a straight
   segment) are doubled knots, so they stay exactly sharp and straight runs
   stay exactly straight. `output = cubic_bezier` returns the fitted pieces
   instead.

A point with `straight = true` makes the segment from the previous point
exactly straight; this is the Shift modifier. A stroke that ends within
`close_radius` of its start (and is at least four times as long) becomes a
closed loop. Its tail inside the radius is dropped and a corner-free loop
gets one tangent across the seam.

`StrokeCapture` holds the state of one drag:

- `begin(hit)`, `move(hit, straight)` and `end()` drive it by hand.
- `update(actions, bindings, ground)` drives it from G5 input actions. The
  draw action (`stroke.draw`, best given a `drag` trigger so clicks don't
  draw) starts at the ground under its press point. `stroke.straight` held
  makes the segment follow the pointer straight. Release fits the stroke;
  `take_result()` returns it once.
- `ground` maps a viewport pixel to a ground hit (your raycast).

```json
{"name": "stroke.draw", "type": "button", "context": "build", "trigger": {"drag": 4}, "bindings": ["mouse:left"]},
{"name": "stroke.straight", "type": "button", "context": "build", "bindings": ["key:left_shift", "key:right_shift"]}
```

## Editor

Select an entity with a `spline` (Add Component → spline). The viewport
then draws the curve and its handles. Bézier handles are squares tied to
their anchors.

- Drag a point to move it in the horizontal plane at its height.
- Ctrl+click the curve to insert a point. A Bézier segment is split without
  changing its shape.
- Alt+click a point to delete it. A deletion that would leave too few
  points is refused.
- **Draw stroke** (bottom-left of the viewport) switches to drawing. A
  left-drag sketches on the plane at the entity's height, Shift draws
  straight segments, and the fitted stroke replaces the spline. Its
  network, id and ground settings are kept.

The Inspector edits the kind, closed, ground following, network, id and
the width / height of every point. It shows the length, or why the
spline is invalid.

## Remote, MCP and Lua

| Remote method | MCP tool | What it does |
|---|---|---|
| `spline.get` | `spline_get` | Spline + length, segments, world bounds |
| `spline.set` | `spline_set` | Merge-edit (or add) a spline; validated whole |
| `spline.sample` | `spline_sample` | Evenly spaced world samples, optional frames and draping |
| `spline.closest` | `spline_closest` | Closest point (3D or XZ) |
| `spline.crossings` | `spline_crossings` | Crossings with another spline, or self-crossings |
| `spline.offset` | `spline_offset` | Offset polyline (distance, width factor, trimming) |
| `spline.network` | `spline_network` | Nodes and edges of a curve network |
| `spline.fit` | `spline_fit` | Fit a stroke into a spline, optionally written to an entity |

Parameters and replies are listed in [`remote-control.md`](remote-control.md).
Lua has a `spline` table: `spline.length`, `spline.at`, `spline.sample`,
`spline.closest` and `spline.fit`; see [`scripting.md`](scripting.md#spline).
