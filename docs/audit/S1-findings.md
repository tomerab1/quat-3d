# S1 bug audit — findings

Static review, 2026-11-27, in three parts: (A) RHI / renderer / frame loop,
(B) scene / physics / nav / net / animation / terrain, (C) editor / assets /
glTF / scene IO. Each finding is re-verified against the code before it is
fixed. **Status:** `open` · `verified` (re-checked, not yet fixed) · `fixed
(<commit>)` · `rejected` (with reason).

Severity: **H** crash, corruption, exploit or broken core feature ·
**M** wrong behaviour in a common path · **L** edge case.

## B — scene, physics, nav, net, animation, terrain

| # | Sev | Finding | Where | Status |
|---|---|---|---|---|
| B1 | H | Host never simulates the remote player's input but acks it (the local keyboard overwrites `cc->move` before `character_system`; multiple inputs per poll collapse; ack sent before stepping); client `pending_` input queue is unbounded | `main.cpp` net block, `net/prediction.cpp` | open |
| B2 | H | Untrusted `PlayerInput.move` applied unvalidated — huge values = speed hack, NaN poisons Jolt and replicates | `main.cpp`, `prediction.cpp` `decode_input` | open |
| B3 | H | `net_id` = FNV(name): duplicate names collide (physics demo's 12 "box" entities converge to one pose) | `main.cpp` assign, `replication.cpp` | open |
| B4 | M | Client runs local physics on replicated bodies (fights snapshots); world pose written into `local` (wrong for children); applied after the draw list is built (one frame stale) | `replication.cpp` `apply`, `main.cpp` | open |
| B5 | M | Predicted character position overwritten each frame by the local `CharacterVirtual` | `main.cpp`, `physics_system.cpp` | open |
| B6 | H | Jolt bodies never removed when an entity / RigidBody / Collider is destroyed (ghost colliders, duplicate bodies, leaks across Play/Stop in demo modes) | `physics_system.cpp` | fixed (S1.3, pending CI) — still open: demo-mode Stop doesn't reset dynamic bodies; inspector edits to Collider/RigidBody fields don't rebuild the body |
| B7 | M | Fixed-step accumulator keeps the excess after the 8-step cap: after a hitch the sim runs ~4x fast, and never recovers below 15 fps | `physics_world.cpp` `update` | fixed (S1.1, pending CI) |
| B8 | M | Terrain height-field bodies go stale / leak when terrain regenerates or streaming toggles during Play | `main.cpp` terrain block | open |
| B9 | M | Height-field padding duplicates edge heights → flat collision shelf over the neighbouring streamed tile | `physics_system.cpp` padding | open |
| B10 | H | Behaviour-tree JSON with wrong types aborts the process (throwing accessors, `-fno-exceptions`) | `behavior.cpp` | fixed (S1.4) |
| B11 | H | Clip samplers index `output` with indices derived from `input` — out-of-bounds read on malformed glTF | `clip.cpp` | fixed (S1.1; anim tests pass locally) |
| B12 | M | Play/Stop drops `NetReplicated` (snapshot doesn't capture it) | `scene_snapshot.cpp` | fixed (S2.2) |
| B13 | M | `Scene::set_parent` accepts self-parenting and cycles → entity drops out of transform propagation; delete recurses forever | `scene.cpp` | fixed (S1.1, pending CI) |
| B14 | L | `character_system` / kinematic nav agents discard rotation, scale and parent space | `physics_system.cpp`, `nav_agent_system.cpp` | open |
| B15 | L | Replication snapshot `uint16` count wraps; large keyframes exceed GNS send limit silently; send accumulator drift | `replication.cpp` | open |

Checked and found sound: packet decoders are bounds-safe; GNS init/refcount;
`glm::slerp` shortest path; erosion border fade keeps seams exact; Play-start
static body emplace; Recast cleanup on failure; Jolt layer filters.

## A — RHI, renderer, frame loop

| # | Sev | Finding | Where | Status |
|---|---|---|---|---|
| A1 | H | `presented` only increments when `QUAT_MAX_FRAMES` is set → no TAA jitter, frame index always 0, demo cube static, no frame stats, `engine.info` frame 0 | `main.cpp` end of loop | verified — **fix together with A2 + A14**: enabling jitter while history is broken makes the image shimmer |
| A2 | H | TAA history ping-pongs per frame slot, so each slot always reads a never-written (black) history image; validity not reset on resize | `taa_pass.cpp`, `main.cpp` | open |
| A3 | H | First swapchain write (TAA copy, ALL_TRANSFER) isn't ordered after the acquire semaphore (waits only COLOR_ATTACHMENT_OUTPUT); imported images start at stage NONE | `main.cpp` submit, `render_graph.cpp` import | fixed (S1.6, pending CI) |
| A4 | H | Render graph emits no barrier for a second reader in an uncovered stage (hdr: compute write → exposure barrier covers COMPUTE only → tonemap fragment read unsynchronised) | `render_graph.cpp` | fixed (S1.6, pending CI) |
| A5 | H | Transient image pool never evicts → VRAM grows with every resize / viewport drag | `render_graph.cpp` pool | fixed (S1.5, pending CI) — images idle for 4 resets are freed |
| A6 | M | Default `DirectionalLight` (0,-1,0) is parallel to lookAt's up → NaN light view, shadows vanish | `main.cpp` shadow setup | open |
| A7 | M | Bloom composite does read-modify-write on hdr but declares write-only access | `bloom_pass.cpp`, `bloom_composite.slang` | fixed (S1.6, pending CI) |
| A8 | M | Descriptor buffers allocated per frame and per draw (VMA alloc + rebind per draw) | mesh/transparent/skinning/bloom/lighting/tonemap/taa/exposure passes | open (perf → R5) |
| A9 | M | `vkQueueWaitIdle` inside the frame loop for terrain tile uploads and TAA/transparent resizes | `gpu_allocator.cpp` one-time commands | open (perf → R1) |
| A10 | M | Storage images declared without format and `shaderStorageImageWriteWithoutFormat` never enabled; TAA history uses BGRA8 as a storage image (optional support) | shaders, `CompileShaders.cmake`, `device.cpp` | open (plausible) |
| A11 | M | Auto-exposure state duplicated per frame slot → doubled time constant + alternating flicker | `exposure_pass.cpp`, `main.cpp` | open |
| A12 | M | Per-slot skinning GPU buffers never released for deleted entities / scene loads | `main.cpp` `skin_gpu` | open |
| A13 | M | Normals/tangents transformed by the model 3x3 (wrong under non-uniform scale; tangent sign under mirroring) | `gbuffer.slang` | open |
| A14 | M | TAA reprojects with jittered matrices → resampling blur + swimming | `main.cpp`, `taa.slang` | open (with A1/A2) |
| A15 | L | Mapped host buffers written without flush/coherence check; pipelines duplicated per frame slot | `gpu_allocator.cpp`, `descriptor_buffer.cpp`, `main.cpp` | open (plausible) |

Checked and found sound: fence/acquire ordering and per-image semaphores,
swapchain recreate, atmosphere/IBL descriptor rings, terrain tile retirement,
material UBO std140 layout and push-constant sizes, skinning→vertex barrier,
screenshot readback barriers, AssetManager-owned mesh lifetime.

## C — editor, assets, glTF, scene IO

| # | Sev | Finding | Where | Status |
|---|---|---|---|---|
| C1 | H | glTF attribute accessors (NORMAL/UV/TANGENT/JOINTS/WEIGHTS) written by index into arrays sized from POSITION's count — heap overflow on a malformed file; `fastgltf::validate` never called | `gltf_loader.cpp` `extract_primitive` | fixed (S1.2, pending CI) |
| C2 | H | glTF cross-references (accessor, node child, joint, texture, image, material, mesh indices) used without range checks | `gltf_loader.cpp` | fixed (S1.2, pending CI) |
| C3 | H | Cyclic glTF node graphs / joint hierarchies recurse forever (stack overflow) | `gltf_loader.cpp` `build`, `node_global_of`; `skeleton.cpp` | fixed (S1.2, pending CI) — node graph must be a forest |
| C4 | H | Animation sampler output count never checked against input count (same defect as B11, loader side) | `gltf_loader.cpp` `extract_animation` | fixed (S1.1, pending CI) |
| C5 | M | Index values and JOINTS_0 values never range-checked → GPU out-of-bounds reads (possible device loss) | `gltf_loader.cpp`, `skinning.slang` | partly fixed (S1.2): vertex indices checked; JOINTS_0 vs skin joint count still open |
| C6 | H | `load_scene` aborts / UB on malformed scene JSON — after the live scene was already cleared | `scene_io.cpp` | fixed (S2.2) — codec reads + staging scene swapped in only on success |
| C7 | H | = B10 (behaviour-tree JSON aborts) | `behavior.cpp` | fixed (S1.4) |
| C8 | H | = B6 (deleting entities / removing Collider or RigidBody in Play leaves live Jolt bodies) | `physics_system.cpp`, editor | fixed with B6 (S1.3) |
| C9 | M | = B12, plus `load_scene` also drops `NetReplicated` | `scene_snapshot.*`, `scene_io.cpp` | fixed (S2.2): snapshot + scene files both keep it |
| C10 | M | If Play fails to create the physics world, the auto-added static RigidBodies stay in the edit scene | `main.cpp` play transition | open |
| C11 | M | Save/load loses CharacterController, NetReplicated, Animator speed/looping, and meshes without MeshSource (editor Duplicate copies too little; demo meshes untagged) | `scene_io.cpp`, `scene_hierarchy.cpp`, `main.cpp` | partly fixed (S2.2): CharacterController, NetReplicated, Animator speed/looping now saved; Duplicate + untagged demo meshes still open |
| C12 | M | On load, skin/animator re-attached by mesh key — instanced skinned meshes get the first node's skin/root transform | `scene_io.cpp` | open |
| C13 | M | Save allowed during Play writes simulated state + auto-added bodies as authored data | `main.cpp` save handling | open |
| C14 | M | Picking: ray origin inside an AABB returns t=0, so an enclosing mesh (rooms, buildings) always wins the click | `editor.cpp` `ray_aabb` | open |
| C15 | L | Renderer panel: sun "intensity" and bloom "intensity" widgets share an ImGui ID | `inspector.cpp` | open |
| C16 | M | Failed asset loads are cached forever; cache-only lookups in `load_scene` create such permanent failed slots | `asset_manager.hpp`, `scene_io.cpp` | partly fixed (S2.2): load_scene uses cache-only get(); load() still never retries failed slots |

Coverage notes: the play snapshot covers every component except
`NetReplicated` (and the new `Primitive`); every selection consumer checks
`registry.valid()`. Missing inspector editors (CharacterController,
SkinnedMesh, MeshSource, NetReplicated) and drag-to-reparent are feature gaps,
tracked under track E.

## N — navmesh / NPCs (dedicated investigation, 2026-11-27)

The owner reported NPCs "don't work". Ranked by user impact. N2 re-verified
(box top triangle normal = (0,-4,0)).

| # | Sev | Finding | Where | Status |
|---|---|---|---|---|
| N1 | H | Behaviour-tree composites don't remember the running child and leaves aren't reset when pre-empted — `sequence[move_to A, wait, move_to B]` twitches at A forever | `behavior.cpp` | open |
| N2 | H | Box collider triangles wound downward: box tops unwalkable, bottoms walkable → navmesh under box floors, agents sink / walk through walls | `physics_system.cpp` `box_idx` | verified |
| N3 | H | Navmesh never saved/auto-baked; stale after load/terrain change; phantom terrain in bakes; agents keep old paths after re-bake; nav silently skipped without one | `main.cpp`, `nav_agent_system.cpp` | open |
| N4 | H | `move_to` without `y` fails on terrain (query extents); `move_to` never reports failure (no failed flag; settle reports success) | `behavior.cpp`, `navmesh.cpp`, `nav_agent_system.cpp` | open |
| N5 | M | Patrol routes stall after Ctrl+click / point edits / late Shift+click; Stop can't stop a patroller | `nav_agent_system.cpp`, `main.cpp`, `inspector.cpp` | open |
| N6 | M | CharacterController agents have no stuck detection (push in place forever) | `nav_agent_system.cpp` | open |
| N7 | M | Coarse single-tile bake on large terrain (≈2 m cells, raised step height) disagrees with physics; vertex limit; should be a tiled navmesh with fixed cell size | `navmesh.cpp`, `physics_system.cpp` | open |
| N8 | M | `near_camera` always fails in editor Play (no Camera entity) | `behavior.cpp` | open |
| N9 | M | Nav + behaviour trees only tick in editor Play; never in the game build or demo modes | `main.cpp` | open |
| N10 | L | Separation re-normalised (can't slow agents; stacking); kinematic agents pushed off the mesh | `nav_agent_system.cpp` | open |
| N11 | L | Ground click ignores boxes/meshes; 2 m march skips thin ridges | `main.cpp` | open |
| N12 | L | NavAgent entity with a Collider and no RigidBody is baked as a hole and promoted to a static wall | `physics_system.cpp`, `main.cpp` | open (plausible) |
| N13 | L | Leftover retry cooldown delays new targets; failed re-bake keeps the old mesh silently | `nav_agent_system.cpp`, `main.cpp` | open |

Test gaps: nothing bakes through `collect_nav_geometry`; no CharacterController
agents, sequences of `move_to`, pre-emption, unreachable targets, patrol +
Ctrl+click, or main-loop integration (bake / Play / load).
