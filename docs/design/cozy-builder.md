# Cozy Builder: a Tiny Glade-style town builder, and the engine it needs

_Status: design proposal, 2026-11-28 (ROADMAP G16.0), revision 2. It feeds
G16 (the game) and the generic engine items P8, G5, G6.1, G11.1, G15.1,
G17, G18, G19, R9.5, R23, R24, R25 and E1._

**Revision 2 (owner's answers, 2026-11-28):**

- the game lives in **its own repository** and builds against an
  installed, versioned **engine SDK** (P8, §3);
- the setting is a **medieval / fairy-tale village**;
- there are **seasons**, not only a day/night slider and weather (R25, §4.14
  and §5.16);
- ambient life (villagers, sheep, birds) is an optional late phase
  (G16.9);
- the remaining questions were decided by default (§10).

The owner wants to make a game with quat-3d. These are the owner's answers:

- a **cozy town and house builder** with no fail state, and with day/night,
  weather and **seasons** for mood. **"Tiny Glade is a good example of what I
  want."** The setting is a **medieval / fairy-tale village**.
- a **top-down 3D camera tilted about 60°** (not 90°) that pans, zooms and
  rotates;
- **organic, procedural building**: the player draws wall and path curves and
  sketches building footprints, and the game generates the geometry and the
  detail (stones, crenellations, arches, windows, doors, roofs, fences, ivy),
  all adapting to the terrain;
- **exterior only**: no interiors and no cutaway walls;
- **"It shouldn't be baked into the engine."** The engine gains only generic
  capabilities. Every builder rule lives in a separate game project, in **its
  own repository**.

This document has two parts, kept strictly apart. **Part I** covers the
engine: generic capabilities any game could use, each with a ROADMAP ID.
**Part II** covers the game: the `cozy-builder` repository, which builds
against the installed engine SDK and uses only its public headers, Lua and
data. Section 3 sets the rule between them, how the game consumes the
engine, and how CI enforces it.

Contents:

1. [Player experience and core loop](#1-player-experience-and-core-loop)
2. [What Tiny Glade does (research)](#2-what-tiny-glade-does-research)
3. [Two layers: engine and game](#3-two-layers-engine-and-game)
4. [Part I: engine capabilities](#part-i-engine-capabilities)
5. [Part II: the game](#part-ii-the-game-the-cozy-builder-repository)
6. [Assets](#6-assets)
7. [Performance targets](#7-performance-targets)
8. [Testing](#8-testing)
9. [Phasing](#9-phasing)
10. [Decisions](#10-decisions)

Every published technique cited here is also in
[`docs/reading/README.md`](../reading/README.md), under "Procedural
building, curves & geometry", "Seasons, snow & ambient life" and
"Cameras, tools & game architecture".

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

**Seasonal loop (hours, or on demand).** The year turns:

- **Spring**: blossom on fruit trees, and fresh, light-green grass.
- **Summer**: deep greens and flowers.
- **Autumn**: the foliage turns gold and red tree by tree, and leaves fall
  and drift.
- **Winter**: bare branches, and snow settling on roofs, wall tops,
  crenellations and the meadow, but not under the eaves or on swept paths.

The player can let the calendar run at a chosen speed or scrub to a season,
the same way as the time of day.

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

**Decided by the owner (2026-11-28): the game gets its own repository.**
The engine repository (`quat-3d`) ships an SDK. The game repository
(`cozy-builder`) builds against a tagged SDK release and nothing else.

```
┌──────────────── cozy-builder repository (Part II) ────────────────┐
│ game.json · module/ (C++: generators, systems, build.* methods)   │
│ data/ · scripts/*.lua · ui/ · input/ · mcp/ (tools, skills)       │
│ assets/ · tests/ · .github/workflows (builds against an SDK tag)  │
└───────────────▲───────────────────────────────────────▲───────────┘
                │ find_package(quat3d 0.N CONFIG)       │ data, Lua,
                │ public headers + exported targets     │ remote/MCP API
┌───────────────┴──── quat-3d SDK release (P8) ─────────┴───────────┐
│ include/engine/** (stable API) · static libs · bundled deps       │
│ quat_add_game() · quat-cook · quat-pack · slangc · quat-mcp       │
│ engine shaders + Slang interface modules · editor fonts/icons     │
├───────────────────────────────────────────────────────────────────┤
│ engine (Part I): G17 game modules · G5 input · G15.1 camera rig   │
│ G18 splines · R23 geometry + dynamic meshes · R9.5 placement      │
│ G19 terrain edits · R24 decals · R25 seasons · G6.1 UI · G11.1    │
│ saves · E1 command stack · R22 WorldClock · R13 weather · ECS …   │
└───────────────────────────────────────────────────────────────────┘
```

### 3.1 The dependency rule

1. **The engine never depends on a game.** Nothing in the `quat-3d`
   repository names, includes, links or fetches the game. Inside the repo,
   in-tree samples (`games/coin-collector/` from P4, and the SDK consumer
   fixture in §3.5) are leaves too: no engine file depends on them.
2. **A game depends only on the installed SDK**: the stable public headers
   (§3.4), the exported CMake targets, the Lua API (`docs/scripting.md`),
   the remote/MCP API, and the documented data formats. Private engine
   headers (`src/`) are not in the SDK, so they cannot be included.
3. **A game never includes ImGui** (the existing CLAUDE.md rule). Game
   components reach the editor inspector through field tables (§3.6).
4. Engine features are named for what they do, not for the game. The
   engine has `CameraRig`, not `BuilderCamera`, and `Spline`, not `WallPath`.

**CI checks in this repository (P8 + G17.1).** Each one is a CTest test,
so it runs on every PR with the rest of the headless suite.

- **`layering_check`** (`cmake/CheckLayering.cmake`, link graph in `cmake/CheckLinkLayering.cmake`) fails when:
  - any file under `src/ include/ tools/ tests/ assets/ cmake/` refers to
    `games/` or `examples/` in an `#include`, a CMake path or a Lua
    `require`. The only exceptions are the top-level `option`-guarded
    `add_subdirectory` lines;
  - an installed public header includes a header that is not installed, or
    includes a private `src/` path;
  - an engine target's link libraries mention a sample target. A CMake check
    walks `LINK_LIBRARIES`, following P4's `cmake/CheckNoRemoteControl.cmake`.
- **`public_headers_self_contained`**: CMake generates one translation unit
  per installed header that includes only that header. Each TU compiles
  against the *installed* include tree, with only the SDK's documented
  dependency include paths (glm, EnTT, nlohmann_json). Any header that
  needs another header first, a private path or an undeclared third-party
  include fails the test.
- **`sdk_consumer`**: §3.5.

A CI matrix entry configures with `-DQUAT_BUILD_SAMPLES=OFF` and builds and
tests the engine alone. The rule should also go into CLAUDE.md ("Games and
the SDK" section) when P8 lands. That is the owner's edit: agents do not
change CLAUDE.md.

### 3.2 The game repository

- **Name**: `cozy-builder`, under the same GitHub owner as `quat-3d`.
- **The owner must create it** (empty, with a default branch) **and connect
  it to Claude's GitHub app.** The factory cannot create repositories. This
  is the only owner action the plan needs. Once
  it exists, sessions attach it with the repository tools and the
  orchestrator runs game items there exactly as it does here.
- It gets its own `CLAUDE.md`, `docs/STATUS.md`, `ROADMAP.md` (the G16
  phases) and `docs/design/`. **Part II of this document moves there as the
  first commit of G16.1**, and this file keeps Part I and a link.
- **Engine-side milestones stay tracked here**: the ROADMAP's "Games"
  section lists G16's phases with their engine dependencies, so engine
  priorities stay visible in one place.

### 3.3 How the game consumes the engine: an installed SDK package (P8)

**Choice: an installable, versioned CMake package, produced by the release
workflow and consumed with `find_package(quat3d 0.N CONFIG)`.** Engine
developers who need to co-develop can override it with a source checkout
(see the bullets below).

Why a package and not FetchContent of the engine source:

- **The engine's build is heavy and particular**: vcpkg in manifest mode
  with overlay ports and custom triplets (`ports/`, `triplets/`), libc++ on
  Linux, Slang fetched at configure time, and offline shader cooking.
  FetchContent would make every game build, and every game CI run, rebuild
  the engine and its whole dependency tree, and would have to replicate the
  triplets and overlay ports exactly. A prebuilt SDK makes the game build
  about a minute instead of an engine-sized build, and every game uses
  binaries built and tested by the engine's own CI.
- **An installed package enforces the API boundary structurally.** Only
  headers we install exist for the game, so rule 2 cannot be broken by
  accident. With FetchContent, `src/` is on disk next to the game and only
  convention keeps it out.
- **One artefact serves everyone.** The release workflow (S4/P6, #24)
  already builds Linux, Windows and macOS archives per tag. The SDK is one
  more archive per OS from the same build.

What the package contains (`quat3d-sdk-<version>-<os>-<arch>.tar.gz/.zip`):

| Part | Content |
|---|---|
| `include/engine/**` | the stable public headers only (§3.4) |
| `lib/` | engine static libraries (`engine_core`, `engine_scene`, …) |
| `deps/` | the vcpkg installed tree for the SDK's triplet, with every static dependency the engine links (Jolt, SDL3, VMA, Lua, sol2, spdlog, fastgltf, meshoptimizer, …), so the link resolves without the game running vcpkg |
| `lib/cmake/quat3d/` | `quat3dConfig.cmake`, `quat3dConfigVersion.cmake`, exported targets (`quat3d::engine`, `quat3d::editor`, `quat3d::remote`), `Quat3dGame.cmake` with `quat_add_game_module()` / `quat_add_game()`, and `quat3d-toolchain.cmake` |
| `bin/` | `quat-cook`, `quat-pack`, `slangc`, `quat-mcp` |
| `share/quat3d/` | cooked engine shaders, the public Slang interface modules (`lib/*.slang`: material, season and generator interfaces), editor fonts and icons, `scripts/package-release.sh`, licences and third-party notices (#96) |

`quat3dConfig.cmake` calls `find_dependency()` for each public-header
dependency (glm, EnTT, nlohmann_json) against `deps/`, and declares the
remaining libraries as `$<LINK_ONLY:…>` interface links. A game links
`quat3d::engine` and needs nothing else.

**Toolchain match.** The SDK is static C++ with no ABI promise across
compilers, so a game must use the same toolchain family as the SDK build:

- clang with libc++ on Linux;
- MSVC v143 on Windows;
- Apple clang with the same deployment target on macOS.

`quat3d-toolchain.cmake` sets the flags (for example `-stdlib=libc++`) and
records the SDK's compiler ID and version. `quat3dConfig.cmake` refuses a
mismatched compiler with a message that names the expected one, rather than
failing later with link errors.

**Engine source override (development only).** Setting
`QUAT3D_SOURCE_DIR=<checkout>` makes the game's top-level CMake
`add_subdirectory()` the engine instead of calling `find_package`.
Engine-side code installs and exports the same target names, so nothing else
changes. Game CI never uses this mode.

### 3.4 Public API surface, versioning and ABI policy

**Stable API**, installed and covered by semver:

| Area | Headers |
|---|---|
| Game modules | `engine/game/*` (`Registrar`, phases, services, field tables) |
| Core | `engine/core/{error, json_fields, sim_clock, log, command_stack, path_confine, spring, utf8_path}.hpp` |
| Input | `engine/input/{actions, action_map, device_state}.hpp` (G5) |
| Scene and ECS | `engine/scene/{components, scene, scene_io, component_json, instanced_mesh, scatter, ground_placement, camera_framing, navigation, camera_rig, spline}.hpp`, plus the season, decal and dynamic-mesh components |
| Geometry | `engine/geometry/*` (R23.1) |
| Physics | `engine/physics/physics_world.hpp` (queries, bodies, characters) |
| Terrain | `engine/terrain/{heightmap, edits}.hpp` |
| Time, sky, weather | `engine/astro/*`, `engine/scene/world_clock.hpp`, `engine/weather/*` |
| Remote | `engine/remote/rpc_server.hpp` method registration types (through `Registrar`) |
| Project | `engine/project/game_project.hpp` |
| Script | the Lua binding API for modules (`engine/script/module_bind.hpp`) |

**Not part of the API**: `engine/rhi/*`, `engine/renderer/*` (passes),
`engine/cook/*`, `engine/net/*` internals and the editor. Games reach
rendering through components, material and season Slang interfaces, the
`DynamicMeshStore` service and `RenderSettings` (A8). These headers are not
installed. If a game needs one, the need is met by promoting a minimal,
Vulkan-free interface into the stable set (P2's rule), never by shipping the
pass header. Today some public headers include Vulkan (`mesh_asset.hpp`
includes `vulkan/vulkan.h`). P8 moves those out of the installed set or
behind opaque handles, and the self-containment test keeps it that way.

**Versioning** follows Semantic Versioning 2.0.0, from the `v0.1.0` tag
(#24) onward:

- **0.x**:
  - **PATCH** releases never break the stable API, data formats or the
    remote protocol.
  - **MINOR** releases may break the API, but every break is listed in
    `CHANGELOG.md` under "Breaking" with a migration note.
  - Where it is feasible, the old name lives on as
    `[[deprecated("use X; removed in 0.(N+1)")]]` for one minor release.
  - `quat3dConfigVersion.cmake` uses `SameMinorVersion` compatibility, so
    `find_package(quat3d 0.4)` accepts only 0.4.x.
- **From 1.0**:
  - breaking changes only in MAJOR releases;
  - deprecated APIs stay for at least one MINOR release and the next MAJOR
    removes them;
  - `SameMajorVersion` compatibility.
- **ABI**: none promised. Games relink against each release (static
  linking makes that the natural workflow). A plugin ABI is a non-goal
  (§3.6).
- **Version queries**: `QUAT3D_VERSION_{MAJOR,MINOR,PATCH}` macros in
  `engine/version.hpp` and `quat3d::version()` at runtime. The player logs
  the engine version, and `session.hello` reports it with the remote
  protocol version.
- **Data formats are versioned separately and read backwards**: `game.json`
  format, scene file version, save format, chunk versions and
  `rules_version`. The engine reads every older format it shipped. A newer
  format is refused with "needs engine ≥ X".
- **The remote / MCP protocol** keeps method names stable within a minor
  release. Removals go through a deprecation that `rpc.methods` reports.

### 3.5 Catching API breaks here: the SDK consumer fixture

`examples/sdk-consumer/` (outside `src/`) is a tiny native-module game. It
registers one component with a field table, one system, one remote method
with an MCP declaration and one save chunk. It has a one-scene `game.json`
and a GoogleTest file. CI runs these steps on Linux on every PR, and on all
three OSes in the release workflow before upload:

```
cmake --install build --prefix $RUNNER_TEMP/sdk          # the real SDK layout
cmake -S examples/sdk-consumer -B build-consumer \
      -DCMAKE_PREFIX_PATH=$RUNNER_TEMP/sdk \
      -DCMAKE_TOOLCHAIN_FILE=$RUNNER_TEMP/sdk/lib/cmake/quat3d/quat3d-toolchain.cmake
cmake --build build-consumer && ctest --test-dir build-consumer
```

The fixture's CMake never sees the source tree. A header that leaks a
private include, a missing export, an uninstalled dependency or a changed
signature fails here, in the engine PR that caused it, before any game
notices. The `sdk_consumer` CTest wraps these steps so they also run
locally.

### 3.6 Game modules (G17), on top of P4 and G7

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
and façade splits per edit needs native code. Lua stays the right place for
light glue. So a game has two levels, and simple games use only the first:

| Level | What the game provides | Runs in |
|---|---|---|
| **Data + Lua** (today) | `game.json`, scenes, Lua scripts, JSON data | the stock `quat-player` (in the SDK and the releases) |
| **Native module** (G17.1) | a static C++ library that registers components, systems, remote methods, Lua bindings, input maps, UI screens and save chunks | a per-game player and editor built by the SDK's `quat_add_game()` |

**Decided by default: a per-game executable with a native module is fine.**

**Why static linking, not a plugin DLL.** C++ has no stable ABI across
compilers or standard libraries, and the engine is built with libc++ on
Linux, MSVC on Windows and Apple clang on macOS. Plugins would also need
macOS code-signing per dylib. A static module linked into a per-game
executable has neither problem, and `quat-pack --player <exe>` already
accepts any player binary. Hot reload of the C++ module is out of scope.
Iteration speed comes from Lua, data hot reload and fast incremental builds
against the prebuilt SDK.

**Registration is explicit.** `quat_add_game()` generates the call to each
module's registration function. There are no static-initialiser tricks:
linkers drop unreferenced objects from static libraries.

```cmake
# cozy-builder/CMakeLists.txt
cmake_minimum_required(VERSION 3.28)
project(cozy_builder CXX)
find_package(quat3d 0.4 CONFIG REQUIRED)     # version pinned in engine.lock

quat_add_game_module(cozy_builder_module
    SOURCES module/register.cpp module/walls/wall_network.cpp ...
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
  - `physics::PhysicsWorld` queries
  - `TerrainEdits` (G19) and `DynamicMeshStore` (R23.2)
  - `asset::AssetManager`
  - the `WorldClock`, `SkyState` (R22) and `SeasonState` (R25)
  - `WeatherState` (R13)
  - the job pool and the `CommandStack` (E1)

  They are the same objects the engine's own systems use, behind headers
  without Vulkan types (P2 rule).
- `game.json` format 2 adds an optional `"modules": ["cozy_builder"]`. The
  player checks it at boot, so a package never runs on a player built
  without its module. It also adds `"mcp": "mcp"`, the folder of game tool
  descriptions and skills (G17.2), and `"engine": ">=0.4,<0.5"`, the SDK
  range the project was built for. Format 1 files stay valid.

### 3.7 Game MCP tools across repositories (G17.2)

quat-mcp stays the only MCP surface and lives in the engine repository. It
grows **no Rust code per game**, so game tools are *declared by the running
game*, not compiled into quat-mcp:

1. The game module registers `build.place` and the rest with
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
   refused with a logged reason. When the engine restarts with a different
   game, the dynamic set is rebuilt.
4. Game skills (`mcp/skills/<name>/SKILL.md`) are served beside A12's
   built-in skills. quat-mcp finds the project root through P4's
   `project.info`. Skill files stay Markdown, following the "no prose in
   source" rule.
5. **Wiring in the game repo**: its `.mcp.json` runs the SDK's `bin/quat-mcp`
   (the version that matches the engine), and its Claude Code plugin
   manifest lists the game's skills. A Claude Code session in the game repo
   then sees the engine tools and `build_*` together.

Until G17.2 lands, the existing `engine_call` tool can already call any
game method, so phases can ship before it.

### 3.8 The game repository's CI

- **`engine.lock`** pins the SDK version and the SHA-256 of each OS
  archive, for example `version = "0.4.2"`.
- **Workflow `ci.yml`**, on every PR and push:
  1. Download the three SDK archives from the engine's GitHub Release for
     that tag with `gh release download`. `quat-3d` is public, so its
     releases download anonymously and no token or secret is needed.
  2. Verify the checksums, then configure with `CMAKE_PREFIX_PATH` and the
     SDK toolchain file.
  3. Build, then run `cozy_builder_tests` (generation goldens, undo, saves,
     validation).
  4. On Linux, run the golden screenshots under Xvfb and lavapipe with the
     SDK's screenshot script. Artifacts are uploaded on failure.
- **`release.yml`**, on game tags: `quat-pack` (from the SDK) packages per
  OS through the SDK's `package-release.sh`, then smoke-tests the package.
- **SDK bumps**: an engine release triggers a `repository_dispatch` to the
  game repository, or a daily scheduled check runs instead. The game repo
  then opens a PR that updates `engine.lock`, and its CI shows whether the
  new SDK breaks the game. Breaks are fixed in that PR, following the
  engine's CHANGELOG migration notes.
- **Early warning (non-blocking)**: a weekly job builds the game against
  the engine's `master` SDK artifact (a nightly SDK from the engine CI),
  reports to the game repo's STATUS, and fails no PR.
- Engine CI cost does not grow: the engine repository never builds the
  game. `quat-3d` is public (the owner made it public on 2026-11-27 to
  lift the Actions billing limit). If `cozy-builder` is public too, its CI
  runs on GitHub's free standard runners as well.

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
  `world_clock`, `season`, `weather` and `terrain_edits` chunks. Modules add chunks
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
- **Seasons** build on both: R25 (§4.14) derives the season from the clock
  and drives the weather from a climate profile.

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

### 4.14 Seasons (R25)

Seasons are generic: any game with a calendar wants them. The engine owns
the **season parameter, the climate that drives the weather, the seasonal
material machinery and snow accumulation**. Games own the **content**:
which asset turns which colour, blossoms, drops its leaves or swaps to a
bare variant (§5.16).

**R25.1 Season state and climate.**

- A `SeasonState` singleton is derived each tick by `season_system` (after
  `world_clock_system`). Its core field is `year_phase` in [0, 1), where 0
  is the local spring equinox.
- **From the clock**: the phase is the apparent solar ecliptic longitude
  ÷ 360°, shifted by 0.5 in the southern hemisphere. R22.1's Sun ephemeris
  already computes that longitude, so equinoxes and solstices fall on their
  real dates for the `WorldClock`'s latitude.
- **Independent mode**: for games that want a short year without
  fast-forwarding the days, the phase advances at `phase_rate` (years per
  simulated second), so day and year cycles are decoupled. A cozy game can
  use a 10-minute day and a 40-minute year. With `drive_clock_date` set, the
  season writes the `WorldClock`'s calendar date (day of year = phase ×
  year), while `fixed_day` keeps looping the time of day. Sun elevation, day
  length and dusk then follow the season, so winter lamps come on earlier.
  `manual` mode holds a phase that the UI, Lua or MCP set.
- Derived fields for shaders and systems:
  - `phase`
  - four smooth season weights (spring, summer, autumn, winter), with
    overlapping cosine windows
  - `day_length_h`, from `day_almanac()`
  - a per-frame constant block shared by every pass (it lands with
    R25.2, its first consumer)
- A **`Climate`** component holds data curves over the year phase: mean
  temperature and diurnal amplitude, precipitation probability, rain/snow
  split by temperature, and cloud cover.
- With `weather_auto` set, a seeded daily weather sequence drives
  `WeatherState` targets (R13.1). `step_weather` already accumulates
  wetness and snow from precipitation and temperature, so **winter snow
  arrives and melts through R13's existing accumulation**. There is no
  separate snow switch. While a season runs, the accumulation integrates
  world-clock time (`weather_clock_time`: the simulation dt × the clock's
  `time_scale`): in simulated seconds a 10-minute winter would barely
  whiten before spring, while a 12-minute day snows and thaws at the pace
  of its sun. Manual `weather.set` keeps working and suspends
  `weather_auto` until it is re-enabled.
- `season.get / set` over remote, MCP and Lua, and a codec field table
  (A8 style).

**R25.2 Seasonal materials and variants**, through Slang interfaces with no
`#ifdef` variants (CLAUDE.md):

- `lib/season.slang` defines an `ISeasonal` interface that a material
  composes:
  - `apply(inout SurfaceSample s, SeasonInputs in)`, where the inputs are
    phase, weights, a per-instance spread hash and the world position.
  - Implementations:
    - `NoSeason`
    - `ColourRamp`: albedo times a 1D ramp texture sampled at
      `phase + spread`, so trees turn one by one. Ramps encode the
      chlorophyll → carotenoid → anthocyanin progression the botany papers
      model (Braitmaier, Diepstraten & Ertl 2004; Zhou, Dong & Mei).
    - `LeafDrop`: a hashed alpha threshold rises over a phase interval, so
      leaves thin out and vanish (Wyman & McGuire's hashed alpha keeps it
      stable and coverage-correct with R12.11's mips). The depth, shadow
      and velocity passes apply the same test, as the foliage rule in
      CLAUDE.md requires.
    - `Blossom`: blends a blossom albedo and mask layer in over an interval.
    - `SeasonalGrass` for R9.3: colour ramp, height scale, and density
      thinning and flattening under snow.
  - Material JSON gains `"season": {"kind": "colour_ramp", "ramp": …,
    "spread": 0.06}`. The pipeline specialises the material's Slang type,
    as A6 does for material modules.
- **Seasonal variant sets**: a `SeasonalVariants` component on a
  `MeshRenderer` or `InstancedMesh` lists mesh variants by phase interval,
  for example a full oak, a sparse autumn oak, a bare winter oak and a
  snow-laden pine. The render collection picks a variant per instance from
  `phase + spread`. Instances are re-partitioned only when the phase
  crosses a threshold, not every frame (on the GPU with R9.4 later). This
  covers assets whose leaves are not separable cards.
- **Particles hook**: an R10.3 emitter parameter can follow a curve over
  `phase`, for falling leaves in autumn or petals in spring.

**R25.3 Snow accumulation masks.** R13.1 already has a global `snow`
amount and a wet/snow surface layer in the lighting pass. R25.3 makes snow
land only where it physically would:

- A **precipitation occlusion map**: a top-down orthographic depth map
  around the camera (a "shadow map from the sky", tilted by the wind). It
  is re-rendered only when geometry changes (the dynamic mesh revision) or
  the camera moves out of its region, and it is shared with R13's planned
  rain occlusion. A surface point is exposed when nothing lies above it.
  This is the occlusion-based method of Ohlsson & Seipel (SIGRAD 2004),
  refined by Reynolds, Laycock & Day (*Real-time accumulation of
  occlusion-based snow*, The Visual Computer 2015). Fearing's offline model
  (SIGGRAPH 2000) is the physical reference for which surfaces hold snow.
- **Coverage** = `accumulation × exposure × smoothstep(slope thresholds on
  N·up, perturbed by world noise) × material snow_affinity`, where glass is
  0, roof tiles are 1 and walls are about 0.6. It is blended into GBuffer
  albedo, normal and roughness, and melt starts on steep, sun-facing faces
  first. As a result, wall tops, crenellations and roof ridges whiten while
  eaves, gate arches and the ground under trees stay clear.
- **Terrain**: the same coverage, multiplied by a G19 paint channel
  `snow_clear` (swept paths). Grass is flattened and hidden under snow
  through `SeasonalGrass`.
- Thick snow geometry (snow caps on roofs) is game content built with the
  R23 kernel when wanted. The engine provides the mask.

**R25.4 Frozen water** (after R16): ice and frost on water surfaces below
0 °C for a sustained time, with a frost material layer.

---

## Part II: the game (the `cozy-builder` repository)

```
cozy-builder/                     its own repository (§3.2)
  engine.lock                     pinned SDK version + archive SHA-256s
  CMakeLists.txt                  find_package(quat3d) + quat_add_game_module + quat_add_game
  game.json                       format 2: modules ["cozy_builder"], mcp "mcp", engine range
  .github/workflows/              ci.yml, release.yml, sdk-bump.yml (§3.8)
  .mcp.json                       runs the SDK's quat-mcp
  CLAUDE.md, ROADMAP.md, docs/    the game's own process docs; this Part II moves here
  module/                         C++ (public engine headers only)
    register.cpp                  components, systems, build.* methods, Lua, chunks
    intent/                       components + command types
    generate/                     walls/, buildings/, roofs/, paths/, fences/, ivy/
    tools/                        tool state machines (ghost, handles, erase)
    lamps.cpp, moods.cpp
  data/styles/*.json              wall, building, roof, fence and path styles
  data/seasons/*.json             per-asset seasonal variants, ramps, climate (§5.16)
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
- **Moods**: the UI offers:
  - a time slider (`time.set` via `WorldClock`, with `fixed_day = true` so
    the time of day loops while R25's independent year moves the calendar
    date: the sun's path and day length follow the season);
  - a "day cycle speed";
  - a **season dial** with its own speed and a "hold this season" toggle
    (§5.16);
  - weather presets (clear, overcast, rain, snow, fog). With the climate's
    automatic weather on, presets are temporary overrides. The presets are
    `weather.set` bundles defined in a Lua script, so they can be tuned
    without a rebuild. A preset may also adjust look parameters (exposure
    compensation, bloom, grading after R20) through the A8 `RenderSettings`
    table (`render_settings.hpp`, `render.settings.set`), so moods need no
    renderer code in the game.

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
**mood panel** (time slider, day speed, season dial and speed, and weather
presets), and a menu
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
`rules_version`, and the engine's clock, season, weather and terrain-edit
chunks.
Load clears the scene, restores intent, and regenerates with progress
shown, at about 1 s for the target town (§7). When `rules_version` changes,
the same intent may produce slightly different geometry. That is accepted:
intent is what the player cares about. Golden tests catch unintended
changes.

### 5.15 MCP tools and the `build-town` skill

Game methods (`build.*`) are registered by the module with JSON Schemas and
exposed as MCP tools by G17.2. Descriptions live in
`cozy-builder/mcp/tools/*.md`. **MCP calls go through the same
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
`season_set`,
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
   poses. Then show the village in all four seasons (`season_set`), with
   one screenshot each.
6. Iterate, using `build_undo` for anything that looks wrong.

The skill states the cozy constraints (no fail state, prefer adjusting to
refusing) and the coordinate conventions (metres, y up, yaw from +X toward
+Z).

### 5.16 Seasons (game content on R25)

The engine supplies the season state, the climate, the `ISeasonal` material
implementations, variant sets and snow masks. The game supplies the
**data**:

- **The year**: independent mode with `drive_clock_date` by default, with a
  40-minute year at normal speed (10 minutes per season) and a day cycle of
  12 minutes, both adjustable in the mood panel (decided by default). A
  **"follow the real calendar"** option switches R25.1 to clock mode: the
  `WorldClock` runs in real time on today's date, so seasons and day length
  match the player's own. A "hold this season" toggle switches to `manual`,
  so a player who loves autumn can stay there. `Climate`
  (`data/seasons/climate.json`) is a temperate, northern-European-style
  profile: snow is likely from the middle of winter, and there are spring
  showers.
- **Per-asset seasonal variants** (`data/seasons/assets.json`), keyed by
  prop or plant id:

  | Asset class | Spring | Summer | Autumn | Winter |
  |---|---|---|---|---|
  | deciduous trees and bushes | `Blossom` (fruit trees), light-green ramp | deep green | `ColourRamp` gold → red with spread, then `LeafDrop`; falling-leaf emitter | bare variant (`SeasonalVariants`) |
  | conifers | — | — | — | snow-laden variant at high snow |
  | flowers and planters | buds → bloom | full | seed heads | hidden (`LeafDrop`) or evergreen pots |
  | grass and meadow | fresh, short | tall, saturated | straw | flattened under snow |
  | ivy | light green | green | red (Boston-ivy ramp) | sparse |

- **Built geometry gets snow for free** from R25.3. The styles set
  `snow_affinity` per material (roof tiles 1, stone 0.6, timber 0.5, glass
  0). Roof styles can add a generated snow cap (R23 offset of the roof
  surface) above an accumulation threshold. Swept paths come from the path
  stroke's `snow_clear` stamp.
- **Seasonal ambience**: lamp schedules follow the shorter winter days
  automatically: `drive_clock_date` moves the sun's path with the season,
  and §5.9 reads `SkyState`. Audio beds (birdsong in spring,
  wind and crows in winter) switch through G1 buses on season weights.
  Optional decorations (pumpkins in autumn, wreaths in winter) are
  `Decoration`s with a season interval.
- **Frozen ponds** wait for R16 water and R25.4.
- **MCP**: `season_set` (an engine tool) and the `build-town` skill's last
  step, "show it in all four seasons", which captures one screenshot per
  season.

---

## 6. Assets

**Decided: a medieval / fairy-tale village with CC0 kits.** The procedural
approach needs far fewer pieces than a modular kit: materials, a few detail
meshes (window frames, doors, shutters, chimneys), props and foliage.
Stones are generated (R23.1).

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
`cozy-builder/scripts/fetch-assets.sh` downloads pinned archive URLs,
checks SHA-256, extracts only the models in a manifest, and writes
`assets/third_party/LICENSES.md` (source, licence, date). quat-pack ships
only referenced files (P4), and the release notes (#96) list third-party
notices.

**Seasonal coverage.** Seasons need, per plant class:

- leaves that can be dissolved separately from the branches (leaf cards or
  a separate leaf mesh), for `LeafDrop`;
- or an explicit bare or winter variant, for `SeasonalVariants`.

G16.1's import step inventories the nature kit for both. Where a tree has
neither, the game builds a bare variant by stripping its leaf material's
triangles at cook time. When a material uses a dedicated leaf texture, that
is a mechanical `quat-cook` step. Blossom and autumn colours come from ramp
textures we author (small PNG gradients, no kit dependency).

**Decided by default: stylised PBR.** The look is stylised and soft
(low-poly forms, painterly tiling textures, gentle AO), close to Tiny
Glade's warm palette. Everything goes through the PBR path; the
stylisation lives in materials and colour grading (R20).

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
| Season and snow cost | ≤ 0.5 ms GPU for the `ISeasonal` evaluation; ≤ 1 ms for the precipitation occlusion map when it re-renders (not every frame); a variant re-partition on a phase threshold ≤ 2 ms CPU (amortised) | R25.2 / R25.3; GPU partition with R9.4 |

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
- **SDK (P8)**:
  - `public_headers_self_contained` compiles every installed header alone.
  - `sdk_consumer` installs the SDK and builds and tests
    `examples/sdk-consumer/` against it (§3.5).
  - The release workflow runs the consumer on all three OSes before
    uploading.
  - A config test checks that a mismatched compiler and a wrong
    `find_package` version are refused with clear messages.
- **Seasons (R25)**:
  - Equinox and solstice phases match the R22 ephemeris for both
    hemispheres.
  - Independent mode is deterministic for a fixed dt sequence.
  - Climate-driven weather sequences are reproducible per seed, and
    winter's snow arrives and melts through R13's `step_weather`.
  - The `ColourRamp`, `LeafDrop` and `Blossom` CPU references match the
    GPU output in an image test.
  - The variant partition flips only at thresholds.
  - Snow coverage image tests: a roof is white, the ground under an eave
    stays clear, and a swept path stays clear.

**Game** (the `cozy-builder` repository's `tests/`, the `cozy_builder_tests`
executable, CTest, run by its own CI against the pinned SDK; GPU-free except
the screenshot job):

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
  lamps, night, rain, and the four seasons (spring blossom, summer, autumn
  colour, winter snow). The per-pixel comparison uses the job's
  thresholds. Every report looks at the images (LEARNINGS: "Look at every
  render you report on").

---

## 9. Phasing

Engine items first, in the order the game needs them. Each line lists its
dependencies. Items already in flight are marked with their PR.

**Engine**

**Decided by default: G16's engine items take priority after the v0.1.0
release** (tagged once #24 merges).

| ID | Item | Depends on |
|---|---|---|
| P8 | Engine SDK: install rules and exported `quat3d::` targets, stable header set, `quat3dConfig` + version file + toolchain file, bundled dependency prefix, SDK archives in the release workflow, `public_headers_self_contained`, `examples/sdk-consumer` + `sdk_consumer` test, `CHANGELOG.md` with the semver policy | P6/S4 (#24), P4 (#55), v0.1.0 |
| G17.1 | Game modules: `Registrar`, phases, `quat_add_game` (shipped in the SDK), per-game player and editor, `game.json` format 2, `Transient` and `Saved` tags, `layering_check` + `QUAT_BUILD_SAMPLES=OFF` CI entry | P8, S9, P4 (#55), G7 (#22) |
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
| R25.1 | Season state (from the clock, independent or manual) and climate-driven weather | R22.1 (#87), R13.1 (#64) |
| R25.2 | `ISeasonal` material implementations (colour ramp, leaf drop, blossom, seasonal grass), seasonal variant sets, particle hook | R25.1, A6 (#19), R12.11, R9.3 |
| R25.3 | Snow accumulation masks: precipitation occlusion map, coverage, terrain `snow_clear` | R25.1, R13.1 (#64), G19.1 |
| R25.4 | Frozen water | R16, R25.1 |
| G5.2 | Rebinding and per-user persistence (gamepad later, by default) | G5.1, G6.1 |

**Game (G16)**

| ID | Phase | Depends on |
|---|---|---|
| G16.0 | This design | — |
| G16.1 | **Skeleton in the new repository**: the owner creates `cozy-builder` and connects it to Claude's GitHub app. Then: `engine.lock`, CI against the SDK tag, Part II of this document moved there, module registration, the fixed meadow scene (terrain + grass + WorldClock + SeasonState), the camera rig with the owner's numbers, the input map, the tests target, the asset fetch script + licence records and seasonal inventory. `cozy-builder` runs and packages | P8, G17.1, G5.1, G15.1, P4 |
| G16.2 | **Walls**: stroke → `WallRun` → network, junction rules, body sweep with terrain adaptation, stones, crenellations, pillars, colliders, picking, erase, ghost + validation; `build_wall_path`, `build_remove`, `build_query`, `build_validate` | G18.1–G18.3, R23.1, R23.2, R9.5 |
| G16.3 | **Buildings and roofs**: footprints, merge groups, floors, façade rules, door and windows, straight-skeleton and cone roofs, foundation flatten + plinth; towers; `build_house`, `build_tower`, `build_styles` | G16.2, G19.1, R23.1 |
| G16.4 | **Paths, gates, fences and ivy**: path paint + decals + stepping stones, gates where paths cross walls and fences, ivy growth; `build_path`, `build_fence` | G16.2, G19.1, R24, R9.5 |
| G16.5 | **Decoration and mood**: props with surface alignment, foliage brush, lamps on at dusk, window glow, weather presets; `build_place`, `build_paint_foliage` | R9.5, R22.1 (#87), R13.1 (#64), A6 (#19) |
| G16.6 | **Build UI, undo/redo, save/load**: tool bar, style strip, mood panel, menu and slots, autosave; `build_undo` / `build_redo` | G6.1, E1, G11.1 |
| G16.7 | **MCP and skill**: G17.2 declarations for all `build.*` tools, the `build-town` skill, the demo town from a command list, golden screenshots in CI | G17.2, G16.2–G16.6, #20 |
| G16.8 | **Scale and polish**: performance targets (§7) with the stress town, GPU detail generation where budgets miss, grow-in animations, tilt-shift DoF, a packaged release via quat-pack | R9.4, R23.3, R20 (#79), R4 (#82) |
| G16.9 | *(optional, late)* **Ambient life**: villagers and sheep wandering on `NavAgent` with behaviour trees (`PatrolRoute`), birds as boids flocks (Reynolds 1987), navmesh refreshed on edits | R23.2 nav dirtiness, existing nav, R10.3 or instanced birds |
| G16.10 | **Seasons**: the climate profile, per-asset seasonal variants (ramps, leaf drop, blossom, bare variants), snow affinity in styles, roof snow caps, path `snow_clear`, seasonal audio and decorations, the season dial; four-season golden screenshots | R25.1–R25.3, G16.5; runs in parallel with G16.6–G16.7 |

Each game phase ships its own MCP tools with its feature (the A12 rule
"skills ship with the tools they need"). G16.7 adds the cross-cutting
declarations, the skill and the screenshots. Phase IDs are not a strict
sequence: G16.10 (seasons) follows G16.5 and can run beside G16.6–G16.8.
G16.9 stays the optional late phase.

Later, by default and not yet scheduled:

- generated maps (a choice of seeded meadows, after the fixed meadow);
- gamepad support (G5 contexts and G6.1 focus navigation are designed for
  it from the start);
- water, ponds and bridges (after R16, with R25.4 freezing), scheduled
  **before** G16.9 ambient life (decided by default).

---

## 10. Decisions

**Decided by the owner (2026-11-28):**

1. **Repository**: the game gets **its own repository** and builds against
   the installed SDK (§3). The owner creates the empty `cozy-builder`
   repository and connects it to Claude's GitHub app. The factory cannot
   create repositories.
2. **Setting**: a **medieval / fairy-tale village** with Tiny Glade's
   feel, built with the Quaternius and Kenney medieval kits (§6).
3. **Ambient life**: villagers, sheep and birds become an optional late
   phase (G16.9).
4. **Seasons**: a full seasons system (R25 in the engine, G16.10 in the
   game), not only a day/night slider and weather.

**Decided by default (the owner can adjust any of these):**

5. A **per-game executable with a native C++ module** is fine (§3.6).
6. **Terrain**: a fixed meadow first, generated maps later.
7. **Platforms**: desktop first (Linux, Windows, macOS), gamepad later.
8. **Look**: stylised PBR.
9. **Art**: CC0 kits.
10. **Priority**: G16's engine items take priority after the **v0.1.0**
    release (ROADMAP note).
11. **Season pace**: an accelerated year by default (a 40-minute year with
    a 12-minute day), with a "follow the real calendar" option (§5.16).
12. **Order**: water and bridges (after R16) come before G16.9 ambient
    life.

**Owner action needed:** create the empty `cozy-builder` repository and
connect it to Claude's GitHub app. `quat-3d` is public, so the game's CI
needs no token to download SDK releases.
