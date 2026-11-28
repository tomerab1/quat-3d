# Game packaging

How a game made with quat-3d (scenes, Lua scripts, assets) becomes something
a player double-clicks on Linux, Windows or macOS (ROADMAP P4, with the
editor's Build & Export dialog from E7).

```
games/coin-collector/          quat-pack            coin-collector-0.1.0-linux-x64.tar.gz
  game.json          ───────►  validate      ───►     coin-collector-0.1.0-linux-x64/
  assets/scenes/...            collect                  coin-collector      (the player)
  assets/scripts/...           cook                     lib/                (libc++)
  assets/icons/...             manifest                 game.json, manifest.json
                               lay out + archive        assets/  (only what the game uses)
                               smoke test               cooked/shaders/
```

## The game project

A game is a directory with a `game.json` at its root. Everything the game
ships lives under that directory, in the same layout as the engine's own data
root, so the runtime treats both alike:

```
games/coin-collector/
  game.json
  assets/scenes/coin_collector.json     start scene (and any others)
  assets/scripts/coin_collector/*.lua   Lua scripts (G7), relative to assets/scripts/
  assets/icons/coin-collector.png       window / app icon (PNG)
  assets/meshes, textures, audio ...    whatever the scenes reference
```

`game.json` (format 1) is strictly validated: unknown keys, wrong types, bad
paths and a format newer than the engine reads are errors that name the
field. Read through `engine/core/json_fields.hpp`
(`engine::project::load_game_project`).

| Field | Required | Meaning |
|---|---|---|
| `format` | yes | Project format version; `1`. A newer value asks for a newer engine. |
| `name` | yes | Display name: window title default, macOS bundle name. |
| `id` | yes | Reverse-DNS identifier (`dev.quat3d.coin-collector`): macOS `CFBundleIdentifier`; its last segment, lower-cased, names the executable and the package. |
| `version` | yes | `MAJOR.MINOR.PATCH` with an optional `-prerelease` (`0.1.0`, `1.2.0-rc.1`). |
| `start_scene` | yes | Project-relative scene file the player loads at boot. |
| `window` | no | `{title, width, height, fullscreen}`; defaults: the name, 1280×720, windowed. |
| `icon` | no | Project-relative `.png`: window icon everywhere, `.icns` in the macOS bundle. |
| `content` | no | Project-relative files or directories always shipped whole (see below). |

Every path is project-relative with `/` separators, may not be absolute or
contain `..`, and is confined to the project root again on disk (symlinks
included).

## Runtime player vs editor build

One build tree produces all three executables from the same engine libraries
and the same application layer (`src/app/`):

| | `game` (editor build) | `quat-player` | `quat-player-shipping` |
|---|---|---|---|
| Editor UI (ImGui, `ENGINE_EDITOR`) | yes | no | no |
| Starts in | the editor, a demo scene | `game.json`'s start scene, playing | the same |
| Shader hot reload | on | off | off |
| Remote control / MCP | `QUAT_REMOTE=<port>` | `QUAT_REMOTE`, unless the policy below says shipping | compiled out (`QUAT_SHIPPING`): no endpoint, no remote screenshots, not linked against `engine_remote` |
| Finds its data | source tree, or next to the executable | next to the executable (`game.json` there), or `QUAT_GAME=<project dir>` | next to the executable only (`QUAT_GAME` ignored) |
| Packaged by | — | `quat-pack --config development` | `quat-pack --config shipping` (the default) |

Shipping is a property of the binary: a shipping package contains
`quat-player-shipping`, which has no remote-control code at all (the
`quat_player_shipping_has_no_remote` test checks its symbol table).
`quat-player` itself fails closed too: a data root holding `game.json` is a
package, and only its `manifest.json` saying `development` allows remote
control — a missing or invalid manifest means shipping — and `QUAT_GAME` can
lower that policy but never raise it.

`ENGINE_EDITOR` is defined per target, not for the whole build: the editor
build still compiles `quat-player` without it. The player's entry point
(`src/player/main.cpp`) stays thin; `GameBoot` (`src/app/game_boot.hpp`)
finds and validates the project and turns it into the application config
(window, icon, start scene, remote policy). The working directory is set to
the project root, so project-relative paths in scene files resolve the same
in the editor, a development run and a package. Engine-owned data (shader
sources, editor fonts) keeps coming from the engine's data root. On Windows
the player is a GUI-subsystem program: double-clicking it opens no console
(redirect its output to read the log).

## What `quat-pack` produces

```
quat-pack <project dir | game.json> --player <quat-player> --out <dir>
          [--config shipping|development] [--build-id <id>] [--cook auto|off]
          [--quat-cook <exe>] [--shaders <dir>] [--stage-only] [--smoke-test]
```

`quat-pack` (C++, `tools/quat-pack`) does the engine-aware half: it
validates the project, collects the game's files through the engine's own
loaders, cooks them, stages the data root and writes the manifest. It then
hands the staged data and the player to `scripts/package-release.sh` — the
release packaging from P6 — for the per-OS layout and archive, and
optionally to `scripts/smoke-test-package.sh`. The layout half stays a shell
script because it is a sequence of platform tools (`ldd`/`patchelf`,
`dumpbin`/`7z`, `install_name_tool`/`codesign`/`ditto`) that CI already
exercises for the engine's own releases; one script lays out both, so the
game packages inherit every fix to it. On Windows it runs under Git Bash.

| Target | Package |
|---|---|
| `linux-x64` | `<slug>-<version>-linux-x64/` + `.tar.gz`: the player (RUNPATH `$ORIGIN/lib`), `lib/` libc++ runtime, data |
| `windows-x64` | `<slug>-<version>-windows-x64/` + `.zip`: `<slug>.exe`, vcpkg + MSVC runtime DLLs, data |
| `macos-arm64` | `<slug>-<version>-macos-arm64/<Name>.app` + `.zip`: Vulkan loader + MoltenVK in `Frameworks/`, data in `Resources/`, `Info.plist` from `game.json` (name, id, version, icon), ad-hoc signed |

Packages are built for the host OS: each needs that OS's player binary and
its tools. CI builds all three.

Output is deterministic: files are staged and listed in sorted order, the
manifest has no timestamps, and archives are written with fixed ordering,
owners and modification times (`SOURCE_DATE_EPOCH`, default 2000-01-01).
Packaging the same inputs twice gives byte-identical data and Linux archives.

## Which files ship, and cooking

Only what the game uses, found by following references rather than by
globbing the project:

- **Scenes**: the start scene, plus every scene file under a `content` root.
  Each is decoded with the scene codec (`engine::scene::scene_references`),
  which reports the glTF models (`mesh`, `instanced_mesh`), audio clips
  (`audio_source`) and scripts (`script`) it names.
- **Models**: a `.gltf`'s external buffers and images (`uri`s) come along.
- **Scripts**: Lua sources are scanned for `require("mod.name")` and for
  string literals naming a script (`"coin_collector/coin.lua"`) or a project
  file (`"assets/meshes/crate.glb"`), so a script that spawns another
  scripted entity pulls it in.
- **`content` roots**: shipped whole — for files only a running script finds
  (computed names).
- **The icon** and the engine's cooked shaders (`.spv` + reflection `.json`).

Not shipped: shader sources, editor fonts, hidden files, anything outside the
project. A reference that leaves the project, or a referenced file that does
not exist, fails the build with the scene and entity that named it. Scenes
saved with absolute paths inside the project (models spawned over MCP) are
rewritten to project-relative paths in the package.

When `quat-cook` (R17) is available, every glTF is cooked on the way in —
KTX2/Basis textures, meshoptimizer-compressed, quantised meshes — and the
manifest marks it cooked. Without it (or with `--cook off`) models are copied
as authored.

`manifest.json` at the data root lists every shipped data file with its
size, SHA-256 and kind, plus the game's id/name/version, the package config,
the target and the build id.

## Versioning

- The **game version** comes from `game.json` and names the package
  (`coin-collector-0.1.0-linux-x64`). macOS gets it as
  `CFBundleShortVersionString`, and its numeric core as `CFBundleVersion`.
- `--build-id` (CI passes the commit) is recorded in the manifest and
  appended to the package name (`coin-collector-0.1.0-dev.abc1234-linux-x64`).
- `game.json` and `manifest.json` carry a **format** number. The engine
  rejects a newer format with an error that says so, instead of misreading
  it.

## How CI and MCP trigger it

- **CI**: the release workflow's `game` job builds `quat-player` and
  `quat-pack`, packages `games/coin-collector` on Linux, Windows and macOS,
  smoke-tests each package (unpacked under a non-ASCII path, launched
  headless for 30 frames, start scene loaded, screenshot written) and uploads
  it as a workflow artifact — a playable test build per OS.
- **Editor**: File → Build & Export… (Ctrl+B) picks the target (the host
  OS), the output folder and the config, runs `quat-pack` in the background
  with progress and its log, and raises a toast with "Show in folder" when
  done (a failed build's toast reopens the log).
- **MCP**: `project.info` describes the open game project; `project.build
  {target?, output?, config?}` starts the same background job and
  `project.build_status` reports its state, progress, log tail and archive
  path (quat-mcp tools `project_info`, `project_build`,
  `project_build_status`).

## Signing (P7)

Packages are not signed for distribution yet. The macOS bundle is ad-hoc
signed (Apple Silicon refuses unsigned code), so a downloaded copy needs
"Open Anyway" once; the Windows build may meet SmartScreen. P7 adds
Developer ID signing + notarization of the `.app` and Authenticode signing of
the `.exe`, with the credentials as CI secrets. The hand-off point is the
layout step: the bundle is complete and its contents final before
`codesign --sign -`, so P7 replaces that identity (and adds `notarytool` +
`stapler`) without touching `quat-pack`.

## Not yet

- Packaging for another OS than the host's (each OS needs its own player
  build and tools).
- An `.exe` icon resource on Windows (the window icon is set at runtime).
- Packaging from a downloaded editor release (it would need to ship
  `quat-player`, `quat-pack` and the layout scripts).
