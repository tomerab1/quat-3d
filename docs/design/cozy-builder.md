# Cozy Builder: a Tiny Glade-style town builder, and the engine it needs

_Status: design proposal, 2026-11-28 (ROADMAP G16.0). Feeds G16 (the game)
and the generic engine items G5, G6.1, G11.1, G15.1, G17, G18, G19, R9.5,
R23, R24 and E1._

The owner wants to make a game with quat-3d. These are the owner's answers:

- a **cozy town and house builder** with no fail state, and with day/night and
  weather for mood. **"Tiny Glade is a good example of what I want."**
- a **top-down 3D camera tilted about 60°** (not 90°) that pans, zooms and
  rotates;
- **organic, procedural building**: the player draws wall and path curves and
  sketches building footprints, and the game generates the geometry and the
  detail (stones, crenellations, arches, windows, doors, roofs, fences, ivy),
  all adapting to the terrain;
- **exterior only**: no interiors and no cutaway walls;
- **"It shouldn't be baked into the engine."** The engine gains only generic
  capabilities. Every builder rule lives in a separate game project.

This document has two parts, kept strictly apart. **Part I** covers the
engine: generic capabilities any game could use, each with a ROADMAP ID.
**Part II** covers the game: the `games/cozy-builder/` project, which uses
only public engine headers, Lua and data. Section 3 sets the rule between
them and how CI enforces it.

Contents:

1. [Player experience and core loop](#1-player-experience-and-core-loop)
2. [What Tiny Glade does (research)](#2-what-tiny-glade-does-research)
3. [Two layers: engine and game](#3-two-layers-engine-and-game)
4. [Part I: engine capabilities](#part-i-engine-capabilities)
5. [Part II: the game](#part-ii-the-game-gamescozy-builder)
6. [Assets](#6-assets)
7. [Performance targets](#7-performance-targets)
8. [Testing](#8-testing)
9. [Phasing](#9-phasing)
10. [Open questions for the owner](#10-open-questions-for-the-owner)

Every published technique cited here is also in
[`docs/reading/README.md`](../reading/README.md), under "Procedural
building, curves & geometry" and "Cameras, tools & game architecture".

---

## 1. Player experience and core loop

**Fantasy.** You tend a small meadow and something lovely grows there.
Nothing is lost, nothing is scored, and nothing runs out. The joy comes from
small gestures that turn into a lot of detail: one drag makes a stone wall
with a crenellated top that hugs the hill, and a quick loop becomes a cottage
with a tiled hip roof, shuttered windows and a door facing the path.

**Core loop (seconds).**

1. **Gesture**: pick a tool (wall, building, path, fence, decorate, foliage)
   and drag on the ground. A live, low-detail *ghost* follows the cursor.
2. **Generate**: on release, full detail appears with a short grow animation
   (stones settle and roof tiles ripple in). This takes well under a second.
3. **Adjust**: grab a handle to reshape the curve or footprint, change the
   style, height or number of floors, or erase. Everything regenerates
   locally, and undo is always one key away.
4. **Admire**: orbit, zoom, scrub the time of day so that lamps come on at
   dusk, and set rain or snow. Take a screenshot.

**Session loop (minutes).** The player grows a hamlet into a village: walls
around it, a gate where a path crosses a wall, a tower at a corner, gardens,
fences and ivy. Saves are small and instant, and there is autosave.

**Non-goals.** No economy, no needs simulation, no fail state, no interiors,
no grid-snapped catalogue of kit pieces (that was the first brief), and no
multiplayer.

**Soft rules instead of failure.** A placement is never "wrong" in a way that
blocks the player. The game adjusts instead: it snaps an endpoint to a nearby
wall, merges overlapping footprints, raises a plinth on a slope or flattens
the ground. It refuses only geometry it cannot build, such as a footprint
narrower than one door, and it explains why on the ghost.

---

## 2. What Tiny Glade does (research)

Tiny Glade (Pounce Light, released September 2024) is made by two people:
Anastasia Opara (procedural art and generation) and Tomasz Stachowiak
(rendering). The published record is thin. Pounce Light has given **no talk
on its generation internals**. What exists:

| Source | What it says that matters here |
|---|---|
| Stachowiak, **"Rendering Tiny Glades With Entirely Too Much Ray Marching"**, Graphics Programming Conference 2024 (Breda). [video](https://www.youtube.com/watch?v=jusWW2pPnA0) · [GPC 2024 archive](https://graphicsprogrammingconference.com/archive/2024/) | A custom renderer: real-time GI, GPU-driven rendering, tilt-shift depth of field, ray tracing / ray marching, and stable shadows under a continuously moving time of day. |
| 80.lv, **"Tiny Glade Developers on Bevy, Proceduralism, Publishers & Cozy Games"** (interview). [80.lv](https://80.lv/articles/exclusive-tiny-glade-developers-discuss-bevy-proceduralism-publishers-cozy-games) | The game grew out of a weekend procedural-wall prototype in Bevy (Rust) and later moved to its own Vulkan renderer. Opara wanted procedural generation on the GPU, in real time, not offline. She writes every rule herself ("procedural art is about rules", in contrast to generative AI). Paths are painted like a brush, with "no need to fiddle with splines". |
| 80.lv dev-clip write-ups: [procedural wall generator upgraded](https://80.lv/articles/anastasia-opara-s-procedural-wall-generator-upgraded), [… now has pillars](https://80.lv/articles/anastastia-opara-s-procedural-wall-generator-now-has-pillars), [… gets trees & flowers](https://80.lv/articles/a-procedural-wall-generator-by-anastastia-opara-gets-trees-flowers), [procedurally glue buildings together](https://80.lv/articles/this-upcoming-game-lets-you-procedurally-glue-buildings-together), [procedural tower roofs](https://80.lv/articles/a-look-at-procedural-tower-roofs-in-tiny-glade), [procedural windows upgrade](https://80.lv/articles/procedural-windows-in-pounce-light-s-tiny-glade-get-an-upgrade/) | The feature set as shown in development: walls with pillars and growing vegetation, buildings that merge when placed together, tower roofs that adapt to the footprint, and windows placed by rule. These articles show the behaviour, not the algorithms. |
| Opara, **"Functional Symbiosis of Art Direction and Proceduralism"**, CEDEC 2018 (EA SEED, PICA PICA). [PDF](https://media.contentapi.ea.com/content/dam/ea/seed/presentations/cedec-2018-functional-symbiosis-of-art-direction-and-proceduralism.pdf) | Her earlier, published philosophy: hand-authored rules serve art direction, layouts are generated by rule, and the artist keeps control. |
| [Tiny Glade (Wikipedia)](https://en.wikipedia.org/wiki/Tiny_Glade) | Release, studio, and the "no goals, no fail state" design. |

**Consequences for this design:**

- **Intent is small and geometry is derived.** The player authors curves,
  footprints and a few parameters, and rules generate everything else. Our
  data model follows this (§5.1). Saves, undo and MCP tools then all work on
  intent, not on meshes.
- **Hand-authored rules, not solvers.** Nothing published suggests wave
  function collapse or learned models. We use deterministic, rule-based
  generators built on well-published geometry: straight skeletons, CGA-style
  facade splits, sweeps along rotation-minimising frames, and Poisson
  placement. These are cited where used.
- **Gridless.** Townscaper's approach (Stålberg's irregular grid and
  marching-squares-style module selection, from his talk *Organic Towns from
  Square Tiles*, IndieCade Europe 2019, summarised in Game Developer's
  [*How Townscaper works*](https://www.gamedeveloper.com/game-platforms/how-townscaper-works-a-story-four-games-in-the-making))
  is the main alternative. We rejected it because the owner asked for
  Tiny Glade's freehand curves. The idea we keep from it is that topology
  picks the module: wall junctions are classified on a curve graph instead
  of a grid (§5.3).
- **GPU generation where it pays.** Opara's stated aim was real-time GPU
  generation. We start with a CPU generator, which is deterministic, testable
  and needed anyway for colliders and saves. We move the high-volume detail
  (stones, tiles, leaves) to compute once the edit-latency budget asks for it
  (R23.3). The CLAUDE.md "GPU compute first" rule allows the CPU path here:
  generation runs on edits, not every frame.
- **Rendering look.** Tilt-shift DoF and stable shadows under a moving sun
  map onto R20 (#79) and R12's texel-snapped CSM. GI maps onto R15. None of
  these blocks the game.

All algorithms below come from published sources, not from guesses about
Tiny Glade's code.

---

## 3. Two layers: engine and game

```
┌───────────────────────── games/cozy-builder/ (Part II) ─────────────────────────┐
│ game.json · module/ (C++: generators, systems, build.* remote methods)           │
│ data/styles/*.json · scripts/*.lua · ui/*.json · input/*.json · mcp/ (tools,     │
│ skills) · assets/ · tests/                                                       │
└───────────────▲─────────────────────────────────────────────────────▲────────────┘
                │ public headers include/engine/** only               │ data, Lua
┌───────────────┴────────────────── engine (Part I) ──────────────────┴────────────┐
│ G17 game modules · G5 input actions · G15.1 camera rig · G18 splines             │
│ R23 geometry kernel + dynamic meshes · R9.5 placement · G19 terrain edits        │
│ R24 decals · G6.1 runtime UI · G11.1 saves · E1 command stack · R22 WorldClock   │
│ existing: ECS, Scatter/InstancedMesh, Jolt raycasts, NavAgent, RenderSettings    │
└──────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 The dependency rule

1. **The engine never depends on a game.** Nothing under `src/`, `include/`,
   `tools/`, `tests/` (engine tests) or `assets/` includes, links, loads or
   names anything under `games/`. The engine builds, and its tests pass,
   with `games/` deleted.
2. **A game depends only on the public engine surface**:
   `include/engine/**` headers, the engine's CMake targets, the Lua API
   (`docs/scripting.md`), the remote/MCP API, and data formats. A game never
   includes headers from `src/` (private engine headers).
3. **A game never includes ImGui** (the existing CLAUDE.md rule). Game
   components reach the editor inspector through field tables (§4.1).
4. Engine features are named for what they do, not for the game. The
   engine has `CameraRig`, not `BuilderCamera`, and `Spline`, not `WallPath`.

**CI check (part of G17.1).** A CTest test, `layering_check`, runs
`scripts/check-layering.sh`. Like the rest of the headless suite, it runs
on every PR. It fails when:

- any file under `src/ include/ tools/ tests/ assets/ cmake/` contains
  `games/` in an `#include`, a CMake path or a Lua `require`. The only
  exception is the top-level `CMakeLists.txt` line
  `if(QUAT_BUILD_GAMES) add_subdirectory(games) endif()`;
- any file under `games/` includes a header that is not under
  `include/engine/`, its own game directory or a vcpkg dependency, for
  example `"../../src/…"` or a path that resolves into `src/`;
- an engine target's include directories or link libraries mention a game
  target. A CMake check walks `get_target_property(... LINK_LIBRARIES)`,
  following P4's `cmake/CheckNoRemoteControl.cmake` pattern.

A CI matrix entry also configures with `-DQUAT_BUILD_GAMES=OFF` and builds
and tests the engine alone. Game include paths enforce rule 2 structurally:
game targets get only the engine's `PUBLIC` include directory, because
`src/` is a `PRIVATE` include path of the engine libraries. The rule should
also go into CLAUDE.md ("Games" section) when G17.1 lands. That needs the
owner's edit, since agents do not change CLAUDE.md.

### 3.2 Where the game lives: `games/cozy-builder/` in this repository

**Recommendation: in this repository, for now.** Reasons:

- P4 (#55) already set the convention with `games/coin-collector/`: a
  `game.json` project packaged by `quat-pack`.
- The engine has no installable, versioned SDK yet: no
  `find_package(quat)` and no API stability promise. A separate repository
  would pin engine commits by hand and break on every engine refactor (S7,
  S8 and S9 are still moving the code).
- Engine and game work will interleave for months. An engine API change and
  its first game use should land in one PR, with one CI run, reviewed
  together.
- The software factory (worktree agents, MCP sessions) works on one
  checkout. Two repositories double the orchestration.

**Graduate to its own repository** once the engine exports a versioned
CMake package (an "engine as library" follow-up to P4) and the game's art
outgrows the engine repo. The layering check makes the move mechanical.
Large source assets are never committed in either case (§6).

### 3.3 Game modules (G17), on top of P4 and G7

What exists or is in flight:

- **P4 (#55)**: a game is a directory with a strictly validated `game.json`
  (format 1). One stock `quat-player` runs any data-plus-Lua game. `quat-pack`
  follows references and packages per OS, and `project.*` remote/MCP methods
  exist. There is no native game code: Coin Collector is scenes plus Lua.
- **G7 (#22)**: a sandboxed per-scene Lua 5.4 state with instruction and
  memory limits. It has a `Script` component, entity/component access
  through the codec, `physics.raycast`, input, and `script.*` remote
  methods.
- **Registration today**: components, remote methods and quat-mcp tools are
  added by editing central lists (`component_json.cpp`, `remote_bridge`,
  `server/mod.rs`). **S9** already plans per-module registration.
- `RpcServer::add_method(name, summary, handler)` already lets anyone add a
  method at runtime.

Lua alone cannot carry this game. Generating 20k stones, a straight skeleton
and facade splits per edit needs native code. Lua stays the right place for
light glue. So a game has two levels, and simple games use only the first:

| Level | What the game provides | Runs in |
|---|---|---|
| **Data + Lua** (today) | `game.json`, scenes, Lua scripts, JSON data | the stock `quat-player` |
| **Native module** (G17.1) | a static C++ library that registers components, systems, remote methods, Lua bindings, input maps, UI screens and save chunks | a per-game player and editor built by `quat_add_game()` |

**Why static linking, not a plugin DLL.** C++ has no stable ABI across
compilers or standard libraries, and the engine is built with libc++ on
Linux, MSVC on Windows and Apple clang on macOS. Plugins would also need
macOS code-signing per dylib and versioned headers, which we do not have.
A static module linked into a per-game executable has none of these
problems, and `quat-pack --player <exe>` already accepts any player binary.
Hot reload of the C++ module is out of scope. Iteration speed comes from
Lua, data hot reload and fast incremental builds.

**Registration is explicit.** CMake generates the call to the module's
registration function. There are no static-initialiser tricks: linkers drop
unreferenced objects from static libraries.

```cmake
# games/cozy-builder/CMakeLists.txt
quat_add_game_module(cozy_builder_module
    SOURCES module/register.cpp module/walls/*.cpp ...
    REGISTER cozy::register_module)          # engine::game::RegisterFn
quat_add_game(cozy-builder                   # makes cozy-builder (player) and
    PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}  # cozy-builder-editor (ENGINE_EDITOR)
    MODULES cozy_builder_module)
```

```cpp
// include/engine/game/module.hpp (G17.1) — sketch
namespace engine::game {

enum class Phase : std::uint8_t {   // where module systems run in Scene::tick
    input,          // after input sampling (G5), before scripts
    pre_physics,    // gameplay that drives physics
    post_physics,   // after physics_system / character_system
    post_transform, // after transform_system: generators, scatter
    pre_render,     // last chance before render collection
};

struct SystemDesc {
    std::string_view name;           // "cozy.generate", for profiling and logs
    Phase            phase;
    SystemFn         run;            // void(SystemContext&, float dt): stateless
    std::span<const std::string_view> after; // ordering within the phase
};

class Registrar {                    // handed to RegisterFn once at boot
public:
    // Components: a field table (like RenderSettingDesc) gives codec, remote
    // schema, Lua access and inspector rows. No ImGui in games.
    template <class T> void component(std::string_view name, ComponentFields<T> fields);
    void system(const SystemDesc& desc);
    // Remote methods with a JSON Schema for params, and optional MCP exposure
    // (G17.2). Names must not collide with engine namespaces: refused.
    void method(MethodDesc desc);
    void lua_module(std::string_view name, LuaBindFn bind);   // require("cozy")
    void input_actions(std::string_view project_path);        // G5 JSON
    void ui_screen(std::string_view name, std::string_view layout_path); // G6.1
    void save_chunk(SaveChunkDesc desc);                       // G11.1
    [[nodiscard]] Services& services();  // physics, terrain edits, meshes, clock…
};

using RegisterFn = std::expected<void, core::Error> (*)(Registrar&);

} // namespace engine::game
```

- Systems stay stateless free functions (CLAUDE.md). A module's state lives
  in components, singleton components and game-owned stores reached through
  `SystemContext`.
- **Services** are public interfaces to what the application owns:
  `physics::PhysicsWorld` queries, `TerrainEdits` (G19),
  `DynamicMeshStore` (R23.2), `asset::AssetManager`, the `WorldClock` and
  `SkyState` (R22), `WeatherState` (R13), the job pool, and the
  `CommandStack` (E1). They are the same objects the engine's own systems
  use, behind headers without Vulkan types (P2 rule).
- `game.json` format 2 adds an optional `"modules": ["cozy_builder"]`. The
  player checks it at boot, so a package never runs on a player built
  without its module. It also adds `"mcp": "mcp"`, the folder of game tool
  manifests and skills (G17.2). Format 1 files stay valid.

**Game MCP tools without editing quat-mcp (G17.2).** quat-mcp must stay the
only MCP surface, and it must not grow Rust code per game. So game tools are
**declared, not coded**:

1. The module registers `build.place` and the rest with
   `MethodDesc{ name, params_schema (JSON Schema), result_schema, mcp: { tool
   = "build_place", description = "mcp/tools/build_place.md", annotations } }`.
2. The engine's `rpc.methods` returns the schemas and the tool metadata. The
   Markdown description is read from the project's `mcp/` folder at
   registration, validated (size cap, UTF-8) and returned inline.
3. On connect and reconnect, quat-mcp lists the engine's methods and adds a
   dynamic tool for each `mcp` entry. rmcp's `ToolRouter` accepts routes
   built at runtime. The tool's input schema is the method's JSON Schema,
   and the handler forwards the call to the engine. quat-mcp then sends
   `notifications/tools/list_changed`. Tool names must match
   `^[a-z][a-z0-9_]{0,63}$` and must not shadow a built-in tool, or they are
   refused with a logged reason.
4. Game skills (`mcp/skills/<name>/SKILL.md`) are served beside A12's
   built-in skills. quat-mcp finds the project root through P4's
   `project.info`. Skill files stay Markdown, following the "no prose in
   source" rule.

Until G17.2 lands, the existing `engine_call` tool can already call any
game method, so phases can ship before it.

---

## Part I: engine capabilities

Each section is a generic capability with its ROADMAP item. The sections
also note what the game uses each one for, but no game rule lives here.

### 4.1 Input actions (G5)

G7 (#22) hard-codes virtual axes and actions (`move_x`, `jump`, `fire`, …)
in `script_input.hpp`. G5 replaces them with data:

- **Action maps** are JSON assets: `{actions: [{name, type: button | axis1d
  | axis2d, bindings: [...]}], contexts: [...]}`. Bindings cover keys, mouse
  buttons, **mouse delta, wheel, pointer position and screen-edge zones**,
  gamepad buttons and sticks, and chords (`ctrl+z`).
- **Triggers**: pressed, released, held, tap, hold(t), double-click, and
  **drag** (start / update / end with a dead zone in pixels, so a click is
  not a drag).
- **Modifiers**: dead zone, scale, invert, smoothing, and "relative to
  camera yaw".
- **A context stack.** The build tool, camera and UI contexts push and pop.
  The top context consumes input first, and the UI consumes pointer input
  over its widgets (G6.1).
- **Rebinding and per-user save (G5.2)**: the user's overrides are stored
  beside the project's defaults, never inside the game data.
- **Access** from C++ (`input::Actions` service), Lua (`input.action(name)`,
  keeping G7's API), and remote `input.inject` (A10.4 already plans it).
  G7's built-in names become the default map, so existing scripts keep
  working.

**Game use:** `input/cozy.json` defines `camera.pan`, `camera.orbit`,
`camera.zoom`, `tool.primary` (drag), `tool.cancel`, `edit.undo`,
`edit.redo`, the tool hotkeys, and so on.

### 4.2 Camera rig (G15.1: orbit / strategy rig)

A generic `CameraRig` component drives the entity's `Transform` (the
existing `Camera` component stays the lens). A `camera_rig_system` runs
after input and before `transform_system`. The rig is a **target-space**
rig, the standard RTS/builder/orbit design:

```cpp
struct CameraRig {                        // authored
    glm::vec3 focus{0.0F};                // point on the ground the camera orbits
    float     yaw_deg = 45.0F;            // heading, camera_framing.hpp convention
    float     pitch_deg = -60.0F;         // negative looks down
    float     distance = 40.0F;           // metres from focus
    // Limits and feel
    float     min_distance = 6.0F, max_distance = 250.0F;
    float     pitch_near_deg = -40.0F;    // pitch at min_distance …
    float     pitch_far_deg  = -65.0F;    // … and at max_distance (log-distance lerp)
    bool      pitch_follows_zoom = true;  // otherwise pitch is free within limits
    float     yaw_step_deg = 0.0F;        // 0 = free rotation; 45 / 90 = snapping
    float     focus_halflife_s = 0.12F, yaw_halflife_s = 0.10F,
              zoom_halflife_s = 0.15F, pitch_halflife_s = 0.15F;
    float     ground_clearance = 2.0F;    // camera stays this far above the ground
    float     focus_ground_halflife_s = 0.25F; // smoothing of focus height
    RigBounds bounds;                     // rect or circle on XZ, soft margin
    // System-managed: damped current values and velocities
    RigState  state;
};
```

- **Critically damped springs.** Each degree of freedom chases its target:
  focus xyz, yaw, pitch, and **log** distance (so zoom feels uniform at every
  scale). Each uses the exact critically damped spring in its half-life form
  (Holden, *Spring-It-On*, 2021; Lowe, *Critically Damped Ease-In/Ease-Out
  Smoothing*, GPG4 2004). The form is frame-rate independent, never
  overshoots, and is already used by `foot_placement.cpp`. Yaw is damped
  along the shortest arc.
- **Pan**: keys and stick move the target focus in the yaw frame, at a
  speed proportional to distance. **Edge scrolling** comes from a G5
  binding (off in windowed mode by default). **Drag pan** is "grab the
  world": at drag start, raycast the pointer onto the ground (physics
  `cast_ray`, falling back to the plane y = focus.y) and remember the hit.
  Each update, move the focus so the grabbed point stays under the pointer.
  Drag pan bypasses focus smoothing for 1:1 feel and keeps the release
  velocity with a decay, for inertia.
- **Zoom toward the cursor**: scale distance by `pow(1.15, -wheel)` and move
  the focus along the cursor ray so the ground point under the cursor stays
  fixed.
- **Pitch eases with zoom** when `pitch_follows_zoom` is set. Pitch is
  interpolated between `pitch_near_deg` and `pitch_far_deg` on log distance:
  close up it becomes a three-quarter view, far away it approaches top-down.
  This is the common builder framing (see Nesky, *50 Game Camera Mistakes*,
  GDC 2014; Haigh-Hutchinson, *Real-Time Cameras*, 2009).
- **Terrain awareness**: the focus height follows ground height sampled at
  the focus and four neighbours a few metres away, through a damped spring,
  so bumps do not jolt the view. The camera keeps `ground_clearance` above
  the ground under its own position. When it would dip below, the rig
  raises pitch instead of moving the focus. A sphere cast (A10.1 shape cast)
  can add collision against buildings. This is rarely needed above 40°
  pitch, and it is optional.
- **Bounds**: the target focus is clamped into `bounds` with a soft margin.
  After a drag ends, an out-of-bounds focus springs back.
- **Framing**: `camera_rig_frame(bounds_box)` reuses `frame_box()`
  (`camera_framing.hpp`) to compute distance and focus for the current yaw
  and pitch.
- **Control from outside**: remote `camera.rig.get / set / frame` (fields as
  above, plus `immediate: true` to skip smoothing), quat-mcp tools
  `camera_rig_get / camera_rig_set / camera_rig_frame`, and a Lua table on
  the component. Screenshots then come from a deterministic pose.
- **Input**: an optional `CameraRigInput` component names G5 actions
  (`pan`, `orbit`, `zoom`, `drag_pan`) so a game gets the full rig without
  code. A game that wants other controls writes rig targets itself.

G15.2 (third-person follow, sphere-cast collision, lock-on) and G15.3
(cutscene tracks on G18 splines) keep the existing G15 scope.

**Game use:** defaults of pitch about −60° at the default distance, free yaw
(with an option for 45° steps), and bounds equal to the meadow.

### 4.3 Splines and curve networks (G18)

**G18.1 `Spline` component and curve library** (`engine_core`, no GPU):

```cpp
struct SplinePoint { glm::vec3 position{0.0F}; float width = 1.0F;
                     float height = 0.0F; std::uint32_t tags = 0; };
enum class SplineKind : std::uint8_t { centripetal_catmull_rom, cubic_bezier, polyline };
struct Spline {
    SplineKind               kind = SplineKind::centripetal_catmull_rom;
    std::vector<SplinePoint> points;           // entity-local
    bool                     closed = false;
    bool                     follow_ground = true; // y re-projected onto the ground
    float                    ground_offset = 0.0F;
};
// System-managed cache: arc-length table, ground-projected samples, bounds, revision.
struct SplineCache { /* ... */ };
```

- **Centripetal Catmull-Rom** is the default for player-drawn curves. It
  has no cusps or self-intersections within a segment and passes through
  the points (Yuksel, Schaefer & Keyser, *Parameterization and applications
  of Catmull-Rom curves*, CAD 2011). Cubic Bézier is available for authored
  curves.
- **Arc-length parameterisation**: a per-segment adaptive Gauss–Legendre
  table, inverted by Newton steps with a bisection fallback.
  `sample_by_spacing(cache, spacing, phase)` gives evenly spaced frames,
  which every placement-along-curve feature needs.
- **Frames**: `up_locked` (tangent projected to XZ with world up: walls,
  fences) or **rotation-minimising** by the double-reflection method (Wang,
  Jüttler, Zheng & Liu, ACM TOG 2008), for rails on slopes, ivy and
  ribbons.
- **Queries**: closest point (projection with a Newton refinement), curve
  and curve intersections in XZ (segment BVH), offset curves, length, and
  bounds.
- **Ground projection** through a `GroundHeightFn`
  (`navigation.hpp`/`ground_placement.hpp` already define the type). This
  is the same sampler Scatter uses, so streamed tiles work.
- Codec (`spline` component), remote `spline.*`, MCP `spline_*`, and Lua.

**G18.2 Curve networks.** A `CurveNetwork` builds a planar graph from many
splines that share a `network` id. It snaps endpoints within a radius,
splits curves at XZ intersections, and classifies nodes by degree and angle:
end, bend, T, X, and n-way. Edges and nodes get stable ids derived from
their source spline ids and parameters, so a local edit keeps the other ids
(needed for incremental regeneration and undo). This is generic: roads,
rivers, rails and fences all need it.

**G18.3 Stroke capture.** A runtime API turns a pointer drag into a clean
spline. It collects ground hits (G5 drag plus raycast), resamples them by
distance, removes jitter with Ramer–Douglas–Peucker, and fits a piecewise
cubic within a tolerance (Schneider, *An Algorithm for Automatically Fitting
Digitized Curves*, Graphics Gems 1990). It then converts the result to
Catmull-Rom points. Options cover closing loops (endpoint within r of the
start) and straight-line segments while Shift is held. The editor uses the
same API for its spline gizmo (insert, drag and delete handles).

### 4.4 Runtime procedural geometry (R23)

**R23.1 Geometry kernel** (`engine_geometry`: CPU, deterministic, no
graphics dependency, fully unit-tested):

| Area | Content | Basis |
|---|---|---|
| 2D polygons | offset (miter, round), union, difference and intersection, simplify, orientation and repair | [Clipper2](https://github.com/AngusJohnson/Clipper2) (Boost licence; integer coordinates make it robust) |
| Triangulation | polygons with holes | [earcut.hpp](https://github.com/mapbox/earcut.hpp) (ISC) |
| Straight skeleton | weighted, for simple polygons. Produces the faces per input edge and the event heights: the roof primitive | Aichholzer, Aurenhammer, Alberts & Gärtner 1995; Felkel & Obdržálek 1998, with its known flaws corrected per Eder, Held & Palfrader, SoCG 2020; weights for gables per Kelly & Wonka 2011. Our own implementation, because CGAL's package is GPL. |
| Sweeps | a 2D profile swept along G18 frames (open or closed, caps), lofts between profiles, extrusion of polygons with taper | Wang et al. 2008 frames |
| Mesh builder | `MeshData` (vertices in the `asset::Vertex` layout, uint32 indices, submeshes), welding, flat or smooth normals by crease angle, MikkTSpace tangents, UV generators (box, planar, along-curve arc length) | Mikkelsen 2008 |
| Optimisation | vertex-cache and fetch order, optional simplified LOD | meshoptimizer (already a dependency from R17) |
| Rocks and stones | seeded procedural stone meshes: a rounded box or icosphere with layered noise displacement and chipped edges | — |

Everything takes explicit seeds and returns `std::expected`. Robustness is
part of the contract: degenerate input is repaired or refused with a precise
error, never a crash. Fuzz tests run in `tests/geometry/`.

**R23.2 Dynamic meshes.** Today `MeshAsset` is static and uploads block on
a `TransferContext`. Generated content needs meshes that change at runtime:

- A `DynamicMesh` component references a mesh in a `DynamicMeshStore`
  service by id. The game submits `MeshData` (CPU) for an id. The store
  uploads through a **staging ring with a per-frame byte budget**, swaps the
  new buffers in at a frame boundary, and retires the old ones through the
  existing `core::DeferredReleaseQueue` (2 frames in flight). The previous
  mesh keeps drawing until the new one is resident, so nothing pops or
  flickers.
- Dynamic meshes go through the same draw collection as `MeshRenderer`
  (batching, shadows, velocity contract, transparency rules), so every
  renderer feature applies.
- **Per-instance custom data** for `InstancedMesh` and `DynamicMesh`: a
  `vec4` per instance (tint and appear-time), readable by the material
  interface (A6, #19). It drives grow-in animations and variation without
  more meshes or materials.
- **Colliders from generated geometry**: an optional collider mode per
  dynamic mesh (`none`, `boxes` (a list of oriented boxes), `convex`,
  `mesh`). Jolt `MeshShape`s are built on the job pool and swapped on the
  main thread. Static only, which matches the CLAUDE.md `collider_mesh`
  rule. The body owner is the entity, so `cast_ray` hits map back to it.
- **Navmesh dirtiness**: changed bounds mark nav regions for an incremental
  rebake (Recast tiles). Used if villagers (G16.9) arrive.
- **Incremental instance updates**: `InstancedMesh` gains stable instance
  ranges per source key, so a generator can replace one wall segment's
  stones without rewriting the entity's whole instance list or
  re-uploading the whole buffer.

**R23.3 GPU-generated meshes.** Generators can register a Slang compute
module (same interface rules as A6 materials, no `#ifdef` variants) that
writes vertex, index or instance buffers plus indirect draw arguments into
pooled buffers. A render-graph `ComputePass` runs it when its inputs change.
Every GPU generator has a CPU reference implementation that tests compare
against, using the existing GPU-vs-CPU test pattern (R10.2). Use it for
high-volume detail whose result the CPU does not need the same frame.

### 4.5 Placement along curves, on surfaces, and brushes (R9.5)

R9.2's `Scatter` places Poisson-disc or jittered-grid candidates in a
rectangle or circle on the terrain (Bridson 2007), deterministically per
seed, and writes `InstancedMesh::instances`. R9.5 generalises the domain and
the output:

- **Curve domain**: candidates along a G18 spline or curve network at
  `spacing` (1D Poisson, or regular with jitter), with offsets across the
  curve (`[-width/2, width/2]` bands), alignment (tangent, normal, up), and
  start and end margins. This covers fence posts, lamp rows, stepping
  stones and flowers along a wall foot.
- **Surface domain**: area-weighted triangle sampling of a mesh (static or
  `DynamicMesh`), with a normal or face filter (for example, "vertical faces
  only" for ivy roots, "upward faces" for moss), plus raycast projection
  onto colliders.
- **Masks**: exclusion and inclusion by distance to curves, polygons and
  volumes (keep trees off paths and out of houses), and by G19 terrain
  paint channels.
- **Brush layer**: a `ScatterBrush` holds a list of stroke operations (add
  or erase in a circle with radius, density and seed). Each stroke
  generates deterministically from its own seed, so replaying the list
  reproduces the layer, and undo is just popping a stroke.
- Deterministic as today: a candidate's fate depends only on its own
  samples. The ground revision mechanism (`ScatterGround::revision`) also
  fires on G19 terrain edits.

### 4.6 Terrain editing (G19)

Terrain is procedurally generated per tile (`Terrain` component, streaming,
GPU synthesis, Jolt height fields). A builder needs **non-destructive edits
on top of the generator**:

- **G19.1 Edit layers.** An ordered, serialised list of edit operations per
  scene. Each operation is a stamp with a shape (polygon, capsule along a
  curve, or circle) and a falloff:
  - `flatten_to(height or plane)`
  - `raise / lower`
  - `smooth`
  - `paint(channel, weight)`

  Channels are the material weights (path dirt, gravel, garden soil) and a
  grass-density mask. Edits apply after generation, per tile, in order, and
  deterministically. A CPU reference runs alongside a GPU path on R10.2's
  tile images. When an edit changes, only the affected tile regions are
  recomputed. The terrain pass, the Jolt height field, Scatter revisions,
  the grass mask (R9.3) and nav regions are all refreshed.
- `terrain.edit.add / remove / list` over remote, MCP and Lua, and a
  `TerrainEdits` service for modules. Returns the edit id, which undo uses.
- Ground queries (`world.ground_height`, Scatter's sampler, `ground_span`)
  see the edited ground.

### 4.7 Decals (R24)

Deferred, box-projected decals write GBuffer albedo, normal and roughness
with angle fade and sort order (Kim, *Screen Space Decals in Warhammer
40,000: Space Marine*, SIGGRAPH 2012 talks). They are instanced, culled
like instances, and later binned into R10.1's clusters for the forward
pass. **Game use:** worn path edges, moss and damp stains at wall feet, and
puddles when R13 wetness is high.

### 4.8 Runtime UI subset (G6.1)

The minimal subset of G6 that a tools-heavy game needs (not ImGui):

- a retained widget tree from JSON layouts: panel, row and column (flex
  layout), icon button, toggle group, slider, list or grid of thumbnails,
  label, tooltip, and modal;
- theme tokens (colours, radii, spacing), DPI scaling, SDF text with the
  bundled Inter font, and icons from the Lucide set already in
  `assets/fonts/`;
- **input routing**: the UI context sits on top of the G5 stack, and pointer
  input over a widget never reaches the world;
- bindings: widgets bind to actions (`on_click: "tool.select:wall"`) and to
  values exposed by C++ modules or Lua;
- a render pass after tonemap, composited in sRGB (the lesson of #102),
  with gamepad and keyboard focus navigation.

Rich text, animation, localisation tables and screen transitions stay in
the G6 follow-up.

### 4.9 Save / load (G11.1)

- **Slots**: `save.list / write / read / delete`, written atomically
  (temporary file and rename), with format version, game id and version,
  timestamp, play time, a thumbnail (screenshot service), and a payload of
  **chunks**.
- Chunks: the engine provides an `entities` chunk (every entity carrying
  the `Saved` tag, written through the component codec) and the
  `world_clock`, `weather` and `terrain_edits` chunks. Modules add chunks
  with `save_chunk{name, version, write, read, migrate}`. Migrations run in
  order when an older version is read. An unknown chunk is kept and
  re-written untouched.
- A **`Transient` tag** marks entities that scene files and saves skip:
  derived, regenerable content.
- Autosave on an interval and on quit. Loads are validated untrusted input,
  read through `core::JsonFields`.
- The Lua `save` / `load` contract remains G11.2.

### 4.10 Command stack (E1, in `engine_core`)

E1's undo/redo stack becomes an engine library, `core::CommandStack`,
rather than editor-only code. Commands have `apply / revert`, merge rules
for continuous drags, transactions (grouping), a memory budget, and a
"clean" marker for unsaved-changes prompts. The editor (E1, E8.4) and games
use the same class. Remote `edit.undo / redo` reach it.

### 4.11 Picking and physics queries (existing)

`PhysicsWorld::cast_ray()` already returns body, owner (entity id), point,
normal and distance, with owner filtering. A10.1 (#54) adds `LayerMask`
filtering, shape casts and overlap queries. G7 exposes `physics.raycast`
to Lua. Picking is a raycast from the cursor ray (from `camera_system`'s
matrices) against colliders: terrain height fields, R23.2 generated
colliders and prop colliders. The owner field maps a hit to the entity.
Nothing new is needed beyond R23.2's generated colliders. A layer for
"pickable, non-physical" helpers can reuse `Layer::sensor` with
`include_sensors`.

### 4.12 Time of day and weather (existing or in review)

- **R22.1 (#87)**: one `WorldClock` per scene (date, time, place, time
  scale, `fixed_day`), real sun and moon ephemerides, and a per-tick
  `astro::SkyState` (sun and moon direction, elevation, illuminance,
  phase). It drives the `DirectionalLight`. `day_almanac()` gives sunrise,
  sunset and civil dusk. There are `time.get / set / skip` methods and MCP
  tools.
- **R13.1 (#64)**: the `WeatherState` singleton (wind, cloud cover,
  precipitation, wetness, snow), the GPU wind field, wet and snow surfaces,
  and cloud shadows, with `weather.get / set` and MCP tools.
- Lamps react through game systems that read `SkyState`, as plain
  components (§5.9). The engine needs no new item for this.

### 4.13 Rendering at builder scale (existing items)

| Need | Item |
|---|---|
| Many identical stones, tiles, props and plants | R9.1 instanced draws, R9.2 `InstancedMesh` |
| 100k+ instances without CPU culling cost | **R9.4** GPU per-instance culling and LOD (today culling is a CPU loop per instance: `instanced_mesh.hpp`) |
| Hundreds of lamps | R10.1 clustered lights (`max_lights = 4096`), no shadows per lamp |
| Glowing windows and lamp glass | GBuffer emission (A6, #19) plus per-instance custom data (R23.2) |
| Ground cover that avoids paths | R9.3 GPU grass plus the G19 grass mask |
| Foliage wind | R14 and the R13 wind field (#64) |
| Tilt-shift look | R20 DoF (#79). A tilted focal plane is a small follow-up there. |
| Stable sun shadows while time flows | R12 CSM with texel snapping |
| Long-term look | R15 GI tiers, R12 GTAO |

---

## Part II: the game (`games/cozy-builder/`)

```
games/cozy-builder/
  game.json                       format 2: modules ["cozy_builder"], mcp "mcp"
  CMakeLists.txt                  quat_add_game_module + quat_add_game
  module/                         C++ (public engine headers only)
    register.cpp                  components, systems, build.* methods, Lua, chunks
    intent/                       components + command types
    generate/                     walls/, buildings/, roofs/, paths/, fences/, ivy/
    tools/                        tool state machines (ghost, handles, erase)
    lamps.cpp, moods.cpp
  data/styles/*.json              wall, building, roof, fence and path styles
  data/props.json                 free-placement catalogue
  input/cozy.json                 G5 action map
  ui/*.json                       G6.1 layouts
  scripts/*.lua                   light glue: mood presets, tutorial hints
  mcp/tools/*.md, mcp/skills/build-town/SKILL.md
  assets/                         scenes, icons, small CC0 meshes; big kits fetched (§6)
  tests/                          cozy_builder_tests (GoogleTest, CTest)
```

### 5.1 Data model: intent and derived content

**Intent** is what the player authored. It is saved, undone, sent over MCP
and edited. **Derived** content is everything generated from intent. It is
tagged `Transient`, never saved, and rebuilt on load.

| Intent component | Fields (all POD-ish, codec field tables) |
|---|---|
| `BuildId` | stable 64-bit id, assigned from a per-save counter (never the `entt` id) and the seed root |
| `WallRun` | the entity's G18 `Spline` (`network = walls`), `style`, `height`, `thickness`, `top: crenellated / flat / capped`, `seed` |
| `Building` | footprint polygon (closed spline, or polygon points), `floors`, `floor_height`, `style`, `roof: {style, pitch_deg, overhang}`, `door_hint` (optional edge index), `seed` |
| `Tower` | centre, radius, height, `roof: cone / flat crenellated`, style, seed |
| `PathStroke` | spline + width + `style` (dirt, gravel, cobble, stepping stones) |
| `FenceRun` | spline + `style` + post spacing |
| `IvyPatch` | seed point(s) on a wall or building face (BuildId + local position), growth amount |
| `Decoration` | `prop` id, transform, `align: upright / surface`, tint |
| `FoliageLayer` | an R9.5 `ScatterBrush` (stroke list) per plant kind |
| `LampLink` | on lamp decorations: on/off schedule offsets (§5.9) |

Derived entities carry `GeneratedBy{BuildId, part}` and `Transient`. They
hold `DynamicMesh` bodies, `InstancedMesh` detail batches, generated
colliders, `PointLight`s and terrain edit ids. Picking a derived entity
resolves to its intent through `GeneratedBy`.

The game's `world` save chunk holds the intent entities (codec), the id
counter and the generator rules version. Engine chunks carry the clock,
weather and terrain edits. Because the terrain edits are derived from
intent, the game re-issues them on load rather than trusting saved ones, so
they cannot drift apart.

### 5.2 Generation pipeline

```
intent change ─► dirty set (BuildIds + XZ bounds) ─► dependency expansion
   (a wall's neighbours via the network; buildings touching a changed
    footprint; paths crossing a wall → gate)
 ─► job per affected part on the job pool:  pure fn(snapshot of intent,
    ground samples, style data, seed) → GeneratedPart (MeshData, instance
    batches, collider boxes, terrain stamps, lights)
 ─► main thread, post_transform phase: apply results (DynamicMeshStore
    submit, InstancedMesh ranges, collider swap, terrain edits, lights),
    start grow-in animation (per-instance appear time)
```

- **Determinism**: a part's output depends only on its intent, its seed
  (`hash(BuildId, part index)`), the style data, the rules version, and the
  ground it samples. There is no global RNG and no dependence on job
  completion order. Results apply in BuildId order.
- **Incremental generation**: walls generate per network edge, buildings per
  merged footprint group, and detail per segment of about 8 m. Only dirty
  parts regenerate. Tests check that an incremental result equals a full
  regeneration (§8).
- **Threading** follows CLAUDE.md: jobs are pure functions of copied inputs,
  as in `terrain::CpuTileJobs`. The ECS is touched only on the main thread,
  with no locks.
- **Preview while dragging**: the ghost uses a cheap path every frame (wall
  body sweep, footprint prism and roof skeleton, no detail) at under 2 ms.
  Full detail is generated on release.
- Generation that misses its budget spreads over frames. The old geometry
  stays until the new one is ready.

### 5.3 Walls

**Network.** Every `WallRun` spline joins the `walls` curve network (G18.2):
endpoints within 0.75 m snap, crossings split, and nodes are classified.
**Junction rules** pick a treatment from the node's degree and angles,
using the same idea as Townscaper's topology-driven module choice, but on a
graph:

| Node | Rule (style-overridable) |
|---|---|
| end | end pillar, or a slope-down cap if `top = capped` |
| bend < 35° | continuous mitred sweep, no pillar |
| bend ≥ 35° | corner pillar; at ≥ 70° and wall height ≥ 3 m, a **corner tower** suggestion (the ghost offers it, the player confirms) |
| T / X / n-way | central pillar sized to the thickest incoming wall; incoming sweeps trimmed against it |
| touches a building | wall trimmed at the façade, with a bonding course of stones into the corner |

**Body.** The wall's cross-section profile comes from the style (batter, a
plinth course and coping) and is swept along `up_locked` frames (R23.1).
**Terrain adaptation**: the base follows the ground at every sample, and the
foundation extends 0.5 m below the lowest ground under the thickness, so no
gaps show. The top line is the ground profile, low-pass filtered along arc
length (style window, default 4 m), plus the height, with a maximum top
slope. On steep runs the top line steps instead, with a step length taken
from the style.

**Detail** (instances):

- **Stones in courses**: each face is split into courses. Course heights
  are sampled from the style distribution (for example 0.25–0.45 m), and
  stone lengths per course from a second distribution, with joints
  staggered at least 0.15 m from the course below (running bond). Each
  stone is one of the style's procedurally generated stone meshes (R23.1),
  scaled to its slot, inset by a jitter and rotated slightly. Larger
  *quoins* go at ends, corners and openings. The rule is deterministic per
  segment seed. About 30–40 stones per metre of a 3 m wall (both faces).
- **Crenellations**: merlons at a style spacing, fitted to an integer count
  per segment between pillars so that ends are symmetric. They follow a
  stepped top.
- **Openings**: where a `PathStroke` crosses a wall, the wall gets a
  **gate arch** sized to the path width (a round or pointed arch profile,
  voussoir stones placed radially along the arch curve). Style rules can
  add arrow slits at a spacing on tall walls.
- **Vegetation**: R9.5 curve-domain scatter along the wall foot (flowers,
  tufts) and surface-domain moss decals (R24) at the base.

**Colliders**: a chain of oriented boxes per segment, not mesh colliders.
They are cheap and good for picking.

### 5.4 Buildings

**Footprints.** A drag-loop (G18.3 closed stroke) or a rectangle drag
creates a footprint. The polygon is simplified (minimum edge 1.2 m and
angle merging), made counter-clockwise, and repaired with Clipper2.
Footprints narrower than 2.4 m anywhere (an inward offset by 1.2 m that
vanishes) are refused with that reason.

**Merging ("glue buildings together").** Footprints that overlap or touch
(within 0.3 m) **and** have the same floor count and style form a merge
group: their union (Clipper2) becomes one shell with one roof. Footprints
with different heights stay separate shells. The lower one's roof is
trimmed against the taller wall, and a flashing course is added where they
meet. Merge groups are recomputed only for changed footprints and their
neighbours.

**Shell.** The walls are an extrusion of the polygon (R23.1) per floor,
with a plinth course and a floor band from the style.

**Foundation.** The game issues a G19 `flatten_to` stamp over the footprint
offset by 1 m (falloff 2 m) at the **median** ground height under it. It
then measures what remains with `ground_span()` (`ground_placement.hpp`)
and builds a stone plinth from the lowest point to floor level. This is
never refused, but above 3 m of residual relief the ghost turns amber with
"steep: tall plinth".

**Façade rules.** These are CGA-style split grammars (Müller et al.,
*Procedural Modeling of Buildings*, SIGGRAPH 2006; Wonka et al., *Instant
Architecture*, SIGGRAPH 2003), written as data in the building style:

1. Each wall edge of each floor is a façade quad. Split it vertically into
   *corner zones* (a style margin, for example 0.6 m) and a central span.
2. Repeat-split the span into **bays** whose count is
   `round(span / bay_target)`, with the bay width then stretched to fit
   exactly (no partial bays). Spans shorter than one bay get none.
3. **The door** goes on the ground floor: the bay closest to the nearest
   `PathStroke`. Without a path, it goes on the longest edge facing the
   camera's default yaw. `door_hint` overrides both. At most one door per
   shell edge group, more for long shells (one per 12 m of street-facing
   length).
4. **Windows**: each remaining bay gets one window type chosen by a seeded
   weighted choice from the style (casement or arched, with shutters or
   flower boxes). Ground and upper floors use separate tables. Bays closer
   than 0.8 m to a junction with another shell or a wall are left blank.
5. Detail meshes for frames, sills, shutters and boxes are instances from
   the props/kit catalogue (§6). The wall surface gets cut openings, with
   the hole punched into the extrusion before triangulation.

### 5.5 Roofs: straight skeleton, with rule-based shapes for special footprints

**Choice: the weighted straight skeleton for general footprints.** A
rule-based hip or gable library would handle only rectangles and a few L or
T shapes. Players sketch arbitrary polygons and merge buildings into unions,
so the roof must be defined for **any simple polygon**. The straight skeleton
is exactly that: it is the canonical roof over a polygon of walls, with every
eave edge shedding one planar face at a common pitch (Aichholzer et al.
1995). **Weights** (Kelly & Wonka, *Procedural Extrusions*, ACM TOG 2011)
turn hips into gables (weight 0 makes the edge's face vertical) and allow
mixed pitches. Implementation is in R23.1, following Felkel & Obdržálek
1998 with the corrections and robustness practice of Eder, Held & Palfrader
(SoCG 2020). Inputs are snapped to a 1 mm integer grid, events at equal
heights are merged within an epsilon, and degenerate cases are fuzz-tested.
CGAL's skeleton package is GPL, so we write our own.

**Rule-based shapes stay where they beat the skeleton:**

| Footprint | Roof |
|---|---|
| general polygon | skeleton hip; gable ends on the two shortest opposite edges if the style says gable (weights 0); mansard = two skeleton passes (steep lower to an offset line, shallow upper) |
| near-circle (towers: polygon fit radius error < 8 %) | **cone or spire** with flared eaves, the tower-roof behaviour shown in the dev clips |
| very small or thin (< 3 m² or width < 2 m) | lean-to (shed) roof against the nearest taller wall |
| skeleton failure (should not happen; logged) | fallback: hip roof over the oriented minimum bounding rectangle, clipped. The player never sees a missing roof |

Roof surfaces get UVs along the slope (tile rows run along the eaves),
ridge and hip caps swept along skeleton edges, an overhang (eave edges
offset outward before skeletonising, then the roof is trimmed to that
outline), and a chimney per shell by seeded rule. Roof tiles are a material
with a normal map, plus a few instanced "ragged" ridge tiles for
silhouette.

### 5.6 Paths, fences and ivy

- **Paths** are painted like a brush (Opara: no fiddling with splines). The
  stroke becomes a `PathStroke` spline with a width. The game issues G19
  stamps: `paint(path_channel)` along a capsule chain, a slight `smooth`,
  and a grass-mask clear. It adds R24 edge decals and, for the stepping and
  cobble styles, R9.5 curve scatter (stones at spacing, slight yaw jitter,
  seated on the ground). Crossing a wall makes a gate (§5.3).
- **Fences**: R9.5 curve-domain posts at a spacing fitted to segment ends,
  rails swept between posts along rotation-minimising frames, and a gate
  where a path crosses.
- **Ivy**: a growth simulation in the spirit of Luft's *Ivy Generator*
  (2007): a random walk from the seed with a primary direction, adhesion to
  the surface found by raycasts against the generated colliders, and
  gravity. It emits branch curves (swept as thin tubes) and leaf instances
  placed by R9.5 along the branches. Growth amount is a parameter, so the
  ivy can grow in over time. It is deterministic per seed.

### 5.7 Free placement: props, lights and foliage

- **Props** (benches, barrels, wells, carts, flower pots, lamps) come from
  `data/props.json`: a glTF path and node, a pivot, a footprint radius,
  `align: upright | surface`, a variant list, a tint palette and an optional
  `light`. There is **no snapping**. Placement raycasts the cursor
  (`cast_ray` against terrain, generated colliders and props). The prop sits
  at the hit point. With `surface`, its up axis takes the hit normal
  (clamped to 35° for upright-style props), and the yaw follows the drag.
  Wall tops and window sills work automatically because they have
  colliders. Many copies of one prop batch into one `InstancedMesh` per
  prop kind; props with lights are separate entities.
- **Foliage brush**: an R9.5 `ScatterBrush` per plant kind, with radius,
  density, erase, and "paint along a curve" (hedges). Masks keep it off
  paths and footprints and out of walls.

### 5.8 Ghost and validity (soft rules)

The ghost is the cheap preview (§5.2), drawn with a translucent material
whose tint shows the state:

| State | Meaning | Examples |
|---|---|---|
| neutral | will build as shown | |
| **adjusted** (blue) | built, with an automatic change the ghost shows | endpoint snapped to a wall; footprint merged; plinth raised; tower suggested |
| **caution** (amber) | built, but something looks odd | very tall plinth; wall top steps a lot |
| **refused** (red, reason on hover) | cannot build | footprint narrower than 2.4 m; self-intersecting closed loop after repair; outside bounds |

Validation is one pure function shared by the ghost, `build.*` remote
methods (which return the same reasons as structured errors) and tests.

### 5.9 Lamps and the time of day (R22)

- A `Lamp` component on a lamp decoration has `on_below_deg` (default −2°)
  and `off_above_deg` (default +2°, for hysteresis), `delay_s` (seeded,
  0–20 s game time, so lamps come on one by one), and a `fade_s` of 1.5 s.
- `lamp_system` (phase `pre_render`) reads the scene's `SkyState`
  (`sun_elevation_deg`) and ramps the `PointLight` intensity and the
  emissive per-instance value of the lamp glass.
- **Windows glow at night** through the same schedule, per building, with
  seeded per-window variation (some stay dark). The glow is emissive only,
  with no point light per window. One low-intensity `PointLight` per
  building shell keeps hundreds of buildings under the cluster budget (§7).
- **Moods**: the UI offers a time slider (`time.set` via `WorldClock`,
  `fixed_day = true` by default so the village keeps its season), a "day
  cycle speed", and weather presets (clear, overcast, rain, snow, fog),
  which are `weather.set` bundles defined in a Lua script so they can be
  tuned without a rebuild. A preset may also adjust look parameters
  (exposure compensation, bloom, grading after R20) through the A8
  `RenderSettings` table (`render_settings.hpp`, `render.settings.set`),
  so moods need no renderer code in the game.

### 5.10 Styles: the data-driven catalogue

The first brief's "piece catalogue" becomes a **style catalogue**. Styles
are rules and parameters, plus references to detail meshes. All files are
JSON, validated strictly through `core::JsonFields`, and hot-reloaded in
development builds.

```json
{
  "format": 1,
  "id": "wall.fieldstone",
  "kind": "wall",
  "profile": { "thickness": 0.6, "batter_deg": 3, "plinth": { "height": 0.4, "out": 0.08 },
               "coping": "rounded" },
  "top": { "smooth_window_m": 4.0, "max_slope_deg": 18, "step_length_m": 2.0 },
  "stones": { "course_height_m": [0.25, 0.45], "length_m": [0.35, 0.9],
              "min_joint_offset_m": 0.15, "inset_m": [0.0, 0.03],
              "generator": { "kind": "rounded_box_noise", "variants": 8, "roughness": 0.35 },
              "material": "materials/fieldstone.json", "tint_jitter": 0.08 },
  "crenellation": { "merlon_width_m": 0.7, "gap_m": 0.45, "height_m": 0.6 },
  "junctions": { "pillar_angle_deg": 35, "tower_angle_deg": 70 },
  "foot_scatter": [ { "prop": "plant.tuft_a", "spacing_m": 0.8, "band_m": [0.2, 0.6] } ]
}
```

Building styles carry the façade tables (bays, window and door types with
weights per floor, corner zones), roof styles carry pitch, overhang, tile
material and ridge caps, and so on. `build.styles` lists everything, so a
model can discover styles instead of guessing them.

### 5.11 Tools and the build menu UI (G6.1 subset)

| Tool | Gesture | Notes |
|---|---|---|
| Wall | drag a stroke; Shift for straight; drag handles to reshape | height scroll while held; style from palette |
| Building | drag a loop or a rectangle; drag a vertex or edge to reshape | `+`/`−` floors; roof style toggle |
| Tower | click to place, drag the radius | |
| Path | brush stroke | width with `[`/`]` |
| Fence | drag a stroke | |
| Decorate | pick a prop, click or drag to rotate | Alt to align to surface |
| Foliage | brush; Ctrl erases | radius and density sliders |
| Ivy | click on a wall face, drag up to grow | |
| Erase | click or brush over anything | removes the intent; the derived parts go with it |
| Adjust | click any built thing | shows its handles and a small style popover |

UI screens (`ui/*.json`): a bottom **tool bar** (icon buttons with hotkey
tooltips), a **style strip** (a thumbnail grid for the active tool, with
thumbnails rendered by the game at first run and cached), a top-right
**mood panel** (time slider, day speed and weather presets), and a menu
(save slots with thumbnails, settings, and rebinding via G5.2). All input
goes through G5 contexts. The UI consumes pointer input over widgets.

### 5.12 Camera feel

The game ships a `CameraRig` and `CameraRigInput` with the owner's numbers:

- pitch −60° at the default distance (40 m), easing from −40° (close) to
  −65° (far);
- free yaw on Q/E or a right-drag, with an optional 45° step setting;
- pan on WASD or a middle-drag (grab the world), edge scroll in fullscreen;
- zoom from 6 m to 180 m toward the cursor;
- bounds equal to the meadow plus a 20 m margin.

Tilt-shift DoF (R20) focuses at the rig's focus distance when available.

### 5.13 Undo / redo

Commands work on **intent only**, through `core::CommandStack` (E1). A
command is a memento of the intent entities it touches: their codec JSON
before and after, plus created and destroyed BuildIds. Apply or revert
restores the intent, and the normal pipeline regenerates the derived
content. This is the command pattern (Gamma et al. 1994; Nystrom, *Game
Programming Patterns*, "Command") with mementos, which suits small intent
and keeps commands trivially correct.

Rules:

- A drag merges into one command.
- A multi-part action is one transaction ("draw a wall that creates a gate
  in another wall").
- MCP calls are undoable like player actions, and `build.undo` returns what
  changed.
- The history has a budget of 500 commands or 16 MB.

### 5.14 Save / load

G11.1 slots carry the game's `world` chunk (intent and the id counter),
`rules_version`, and the engine's clock, weather and terrain-edit chunks.
Load clears the scene, restores intent, and regenerates with progress
shown, at about 1 s for the target town (§7). When `rules_version` changes,
the same intent may produce slightly different geometry. That is accepted:
intent is what the player cares about. Golden tests catch unintended
changes.

### 5.15 MCP tools and the `build-town` skill

Game methods (`build.*`) are registered by the module with JSON Schemas and
exposed as MCP tools by G17.2. Descriptions live in
`games/cozy-builder/mcp/tools/*.md`. **MCP calls go through the same
validation, generation and undo stack as the player.** There is no "admin"
path that skips the rules.

| Tool (method) | Params (abridged) | Returns |
|---|---|---|
| `build_wall_path` (`build.wall_path`) | `points[[x,z]...]` or `[[x,y,z]...]`, `closed`, `style`, `height`, `top` | BuildId, snapped or adjusted notes, generated stats |
| `build_house` (`build.house`) | `footprint` (polygon) **or** `rect {center, size, yaw}`, `floors`, `style`, `roof {style, pitch_deg}`, `door_hint` | BuildId, merged-with ids, door edge, window count |
| `build_tower` | `center`, `radius`, `height`, `roof` | BuildId |
| `build_path` | `points`, `width`, `style` | BuildId, gates created |
| `build_fence` | `points`, `style` | BuildId |
| `build_place` | `prop`, `position` (x,z; y from ground or surface) or `on {build_id, …}`, `yaw_deg`, `scale`, `align` | BuildId |
| `build_paint_foliage` | `kind`, `circle {center, radius}` or `along {points, band}`, `density`, `erase`, `seed` | stroke id, instances added or removed |
| `build_remove` | `ids[]` or `area {polygon}` | removed ids (undoable) |
| `build_query` | `ids[]` or `area`, `detail: summary / full` | intents (codec JSON), validation state, derived counts, bounds |
| `build_validate` | the same params as a build call | the ghost result without building: state and reasons |
| `build_undo` / `build_redo` | `steps` | what changed |
| `build_styles` | `kind?` | style ids with a short description and the key parameters |

Engine tools complete the set: `camera_rig_set/frame`, `time_set`,
`weather_set`, `screenshot`, `terrain_edit_*`, and `scene_save/load`
(or `save_*` after G11.1).

**`build-town` skill** (`mcp/skills/build-town/SKILL.md`): a workflow for
a model. The steps:

1. Read `build_styles` and ground heights (`world_ground_height`) over the
   meadow.
2. Plan a layout: a central lane, house clusters along it with doors facing
   it, a wall loop with a gate on the lane, a corner tower, gardens, and
   fences.
3. Build it in that order with `build_validate` before each call, and react
   to "adjusted" or "refused" notes.
4. Paint foliage last.
5. Set dusk (`time_set`) and check with `screenshot` from `camera_rig_frame`
   poses.
6. Iterate, using `build_undo` for anything that looks wrong.

The skill states the cozy constraints (no fail state, prefer adjusting to
refusing) and the coordinate conventions (metres, y up, yaw from +X toward
+Z).

---

## 6. Assets

The procedural approach needs far fewer pieces than a modular kit:
materials, a few detail meshes (window frames, doors, shutters, chimneys),
props and foliage. Stones are generated (R23.1).

| Source | Licence (checked 2026-11-28) | Formats | Use |
|---|---|---|---|
| **Quaternius — Medieval Village MegaKit** ([quaternius.com](https://quaternius.com/packs/medievalvillagemegakit.html), [itch.io](https://quaternius.itch.io/medieval-village-megakit)) | **CC0 1.0** (stated on the pack page and the itch.io listing) | glTF, FBX, OBJ (Blend in the paid Source tier) | **Primary**: doors, windows, shutters, chimneys, props. The free Standard tier is a subset of the 304 models; we use only what it contains. |
| **Quaternius — Stylized Nature MegaKit** ([quaternius.com](https://quaternius.com/packs/stylizednaturemegakit.html), [OpenGameArt](https://opengameart.org/content/stylized-nature-megakit)) | **CC0** | glTF, FBX, OBJ | Trees, bushes, flowers and rocks for the foliage brush (the free tier lists 68 models) |
| **Kenney — Fantasy Town Kit** ([kenney.nl](https://kenney.nl/assets/fantasy-town-kit), [OpenGameArt mirror](https://opengameart.org/content/fantasy-town-kit)) | **CC0**: Kenney's [support page](https://kenney.nl/support) states all asset-page game assets are CC0, and each download includes a licence file | the formats on the page could not be confirmed from here; check the zip on import | Secondary: blockout and stylised props, fences, market stalls; a fallback set if Quaternius coverage has gaps |
| **Poly Haven** textures ([licence](https://polyhaven.com/license)) | **CC0** | PNG/EXR PBR sets, glTF models | Tiling materials: plaster, fieldstone, roof tiles, planks, path dirt. A9's `asset_search`/`asset_fetch` (#53) already fetch and record these. |

**Verification note.** This session's network could not fetch the pages
directly. The licences above come from the pages' own text as indexed by
web search (Kenney support, the OpenGameArt listing, the Quaternius pack
pages, the Poly Haven licence and FAQ). G16.1 must re-check each licence
file inside the downloaded archive and record it. Kenney's other kits
(Building Kit, City Kit) suit modern and urban settings, not this one.

**Formats and scale conventions.** The engine uses metres and Y-up, and
glTF 2.0 is Y-up, right-handed, in metres with the asset front facing +Z.
Kits do not always honour metres, so **each kit gets a `scale` and
`forward` entry in `data/kits.json`**, measured on import: the cooker
already records bounds, and a test asserts that a door is 1.9–2.3 m tall
after scaling. Detail meshes are cooked by R17 (`quat-cook`: meshopt,
KTX2). **Large archives are never committed.**
`games/cozy-builder/scripts/fetch-assets.sh` downloads pinned archive URLs,
checks SHA-256, extracts only the models in a manifest, and writes
`assets/third_party/LICENSES.md` (source, licence, date). quat-pack ships
only referenced files (P4), and the release notes (#96) list third-party
notices.

**Style**: the look is stylised and soft (low-poly forms, painterly tiling
textures, gentle AO), close to Tiny Glade's warm palette. Everything goes
through the PBR path; the stylisation lives in materials and colour
grading (R20).

---

## 7. Performance targets

Reference machines are a GTX 1060-class desktop at 1080p, and an M1-class
Mac at 1440p through MoltenVK. The **target town** has 120 buildings, 2 km
of walls, 1.5 km of paths, 6 towers, 400 lamps, 3,000 props and 300k
foliage instances.

| Metric | Target | Lever |
|---|---|---|
| Frame time, target town, any time of day | ≤ 16.6 ms (60 fps), GPU ≤ 12 ms | R9.4 GPU culling and LOD (required above about 50k visible instances), R17.1 mesh LODs, clustered lights |
| Instances (stones + tiles + props + foliage) | ≥ 500k resident, ≥ 150k visible | `InstancedMesh` batches per (mesh, material); R9.4 |
| Point lights | 400 lamps + 120 building glows; ≤ 1 ms light culling | R10.1 clusters (`max_lights` 4096); no lamp shadows |
| Draws after batching | ≤ 1,500 | one material atlas per style; R5 bindless later |
| Ghost update while dragging | ≤ 2 ms CPU per frame | cheap preview path (§5.2) |
| Full detail after release, one wall of 30 m or one house | ≤ 50 ms to first appearance (jobs plus a budgeted upload) | parallel jobs; R23.3 GPU detail if missed |
| Load and regenerate the target town | ≤ 1.5 s | parallel generation; detail streams in after the shells |
| Upload budget | ≤ 8 MB/frame staging | R23.2 ring |
| Save file | ≤ 1 MB for the target town (intent only) | §5.1 |
| Memory | ≤ 1.5 GB GPU, ≤ 1 GB CPU | instancing; generated stones shared |

A `stress_town` generator in the game's test tools builds the target town
from a seed. A perf job (not a unit test) records frame and generation
times through R4 (#82) profiling zones.

---

## 8. Testing

**Engine** (`tests/<module>/`, headless, GoogleTest):

- **Curves (G18)**: arc-length accuracy against brute force, frame
  continuity (no flips on RMF along helices), closest-point property tests,
  stroke fit within tolerance, and network snapping and classification on
  fixed fixtures with stable ids across edits.
- **Geometry kernel (R23.1)**:
  - Straight skeleton on fixtures (rectangle, L, U, T, a star, a concave
    polygon, a near-degenerate polygon with collinear and equal-angle
    events, and weights for gables) plus a **fuzz property suite** on
    random simple polygons. Every face is planar and lies on its edge's
    plane, the faces partition the polygon's area exactly, every input edge
    gets exactly one face, and failures fall back and log.
  - Clipper and earcut wrappers on holes and touching rings.
  - The sweep has no NaNs and has correct winding.
- **Dynamic meshes (R23.2)**: GPU tests on the surface-less device fixture.
  A replacement keeps drawing the old mesh until upload completes, the
  deferred release frees after 2 frames, and colliders swap.
- **Terrain edits (G19)**: CPU vs GPU equality within tolerance,
  order-dependence, and region-limited recompute equal to full recompute.
- **Placement (R9.5)**: determinism per seed, and brush replay equals
  incremental.
- **Camera rig (G15.1)**: spring convergence without overshoot at variable
  dt, grab-pan keeps the grabbed point under the cursor, zoom-to-cursor
  invariance, and bounds.
- **Game modules (G17)**: a fixture module registers a component, a system,
  a method and a save chunk, then boots a headless app. `layering_check`
  runs on every build.

**Game** (`games/cozy-builder/tests/`, the `cozy_builder_tests` executable,
CTest; GPU-free except the screenshot job):

- **Deterministic generation goldens**: for each fixture intent (a wall over
  a hill, a T junction, an L house with a gable, merged houses of equal and
  unequal height, a round tower, a path through a wall, a fence, ivy), the
  quantized derived output (vertex positions rounded to 0.1 mm, instance
  transforms, collider boxes, terrain stamps) is hashed and compared to a
  checked-in hash. A mismatch prints a diff summary. Goldens are updated
  deliberately with `--update-goldens` and a `rules_version` bump.
- **Determinism across threads**: generation with 1 and N workers gives
  identical hashes.
- **Incremental equals full**: random edit sequences (property test) end in
  the same derived state as regenerating from scratch.
- **Undo/redo**: random command sequences, where undo-all returns an empty
  world and redo-all reproduces the hash. Drag merging is covered too.
- **Save/load**: save, clear, load and regenerate gives the same hash.
  Loading an older `rules_version` runs the migrations.
- **Validation**: every refusal reason has a fixture, and the MCP-level
  errors name the field.
- **Façade and roof rules**: bay fitting (integer bays, no partial bays),
  door selection near a path, and the circle detector for tower roofs.
- **MCP**: quat-mcp's mock-engine tests cover G17.2 dynamic tool discovery
  from an `rpc.methods` fixture (schema forwarding, name collisions,
  `tools/list_changed`). The build-town skill is linted against the tool
  list (A12 sync-test pattern).
- **Golden screenshots** on the CI screenshot job (#20). A fixed **demo
  town** is built from a checked-in list of `build.*` commands (the same
  commands MCP would send) and captured at four rig poses × noon, dusk with
  lamps, night, and rain. The per-pixel comparison uses the job's
  thresholds. Every report looks at the images (LEARNINGS: "Look at every
  render you report on").

---

## 9. Phasing

Engine items first, in the order the game needs them. Each line lists its
dependencies. Items already in flight are marked with their PR.

**Engine**

| ID | Item | Depends on |
|---|---|---|
| G17.1 | Game modules: `Registrar`, phases, `quat_add_game`, per-game player and editor, `game.json` format 2, `Transient` and `Saved` tags, layering check + `QUAT_BUILD_GAMES=OFF` CI entry | S9, P4 (#55), G7 (#22) |
| G5.1 | Input action maps, contexts, triggers (drag, hold, double-click), modifiers, Lua and remote access | G7 (#22) |
| G15.1 | `CameraRig` orbit/strategy rig and `CameraRigInput`; `camera.rig.*` and MCP | G5.1 |
| G18.1 | `Spline` component and curve library | — |
| R23.1 | Geometry kernel (polygons, triangulation, straight skeleton, sweeps, mesh builder, stones) | — |
| R23.2 | Dynamic meshes, per-instance custom data, generated colliders, incremental instance ranges | R9.2, R17, A6 (#19) for emissive custom data |
| G19.1 | Terrain edit layers (flatten, raise, smooth, paint, grass mask) | R10.2 |
| R9.5 | Placement along curves, on surfaces, masks, brushes | G18.1, G19.1 (masks) |
| G18.2 | Curve networks | G18.1 |
| G18.3 | Stroke capture and fitting | G18.1, G5.1 |
| E1 | Command stack in `engine_core` (editor and games) | — |
| G6.1 | Runtime UI subset | G5.1, #102 (sRGB composite) |
| G11.1 | Save slots, chunks, migrations, autosave | G17.1 |
| G17.2 | Declared game MCP tools and game skills in quat-mcp | G17.1, A12 (#83) |
| R24 | Deferred decals | R12 |
| R23.3 | GPU-generated meshes | R23.2, A6 (#19) |
| R9.4 | GPU instance culling and LOD (existing item) | R6 |
| G5.2 | Rebinding and per-user persistence | G5.1, G6.1 |

**Game (G16)**

| ID | Phase | Depends on |
|---|---|---|
| G16.0 | This design | — |
| G16.1 | **Skeleton**: `games/cozy-builder` project, module registration, meadow scene (terrain + grass + WorldClock), the camera rig with the owner's numbers, the input map, the tests target, asset fetch script + licence records; `cozy-builder` runs and packages | G17.1, G5.1, G15.1, P4 |
| G16.2 | **Walls**: stroke → `WallRun` → network, junction rules, body sweep with terrain adaptation, stones, crenellations, pillars, colliders, picking, erase, ghost + validation; `build_wall_path`, `build_remove`, `build_query`, `build_validate` | G18.1–G18.3, R23.1, R23.2, R9.5 |
| G16.3 | **Buildings and roofs**: footprints, merge groups, floors, façade rules, door and windows, straight-skeleton and cone roofs, foundation flatten + plinth; towers; `build_house`, `build_tower`, `build_styles` | G16.2, G19.1, R23.1 |
| G16.4 | **Paths, gates, fences and ivy**: path paint + decals + stepping stones, gates where paths cross walls and fences, ivy growth; `build_path`, `build_fence` | G16.2, G19.1, R24, R9.5 |
| G16.5 | **Decoration and mood**: props with surface alignment, foliage brush, lamps on at dusk, window glow, weather presets; `build_place`, `build_paint_foliage` | R9.5, R22.1 (#87), R13.1 (#64), A6 (#19) |
| G16.6 | **Build UI, undo/redo, save/load**: tool bar, style strip, mood panel, menu and slots, autosave; `build_undo` / `build_redo` | G6.1, E1, G11.1 |
| G16.7 | **MCP and skill**: G17.2 declarations for all `build.*` tools, the `build-town` skill, the demo town from a command list, golden screenshots in CI | G17.2, G16.2–G16.6, #20 |
| G16.8 | **Scale and polish**: performance targets (§7) with the stress town, GPU detail generation where budgets miss, grow-in animations, tilt-shift DoF, a packaged release via quat-pack | R9.4, R23.3, R20 (#79), R4 (#82) |
| G16.9 | *(optional, owner's call)* **Ambient life**: villagers and sheep wandering on `NavAgent` with behaviour trees (`PatrolRoute`), navmesh refreshed on edits | R23.2 nav dirtiness, existing nav |

Each game phase ships its own MCP tools with its feature (the A12 rule
"skills ship with the tools they need"). G16.7 adds the cross-cutting
declarations, the skill and the screenshots.

---

## 10. Open questions for the owner

1. **Repository**: in-repo `games/cozy-builder/` now and its own repository
   later (recommended, §3.2), or its own repository from day one?
2. **Native module vs Lua-only**: is a per-game executable
   (`cozy-builder`, built with the C++ module) acceptable, rather than the
   stock `quat-player`? The generators need native speed.
3. **Setting and era**: medieval or fairy-tale village (Tiny Glade-like,
   fits the CC0 kits), or something else (Nordic, Mediterranean, modern
   cottage)? This sets the first style catalogue.
4. **Seasons and climate**: a fixed summer day with a time slider
   (`fixed_day`), or a running calendar with seasons and snow in winter
   (R22 + R13)?
5. **Villagers and animals** (G16.9): wanted, or a still life?
6. **Water**: ponds, streams and bridges (Tiny Glade has them) need R16
   water and a bridge rule. Add them as a later phase?
7. **Terrain**: one fixed meadow per save, a choice of generated maps, or
   player terrain sculpting (G19 makes raise and lower brushes cheap to
   expose)?
8. **Platforms**: desktop only (Linux, Windows, macOS), or gamepad and
   Steam Deck support from the start (G5 contexts and G6 focus navigation
   would then be tested on it)?
9. **Look**: stylised PBR as proposed, or a more painterly NPR shading
   pass?
10. **Art budget**: CC0 kits only, or a commissioned or bespoke kit later
    (the style catalogue keeps either possible)?
11. **Priority**: should the engine items G16 needs (§9) go ahead of the
    other G and R items once the current S → A → P priority list allows?
    The ROADMAP note proposes this without reordering anything yet.
