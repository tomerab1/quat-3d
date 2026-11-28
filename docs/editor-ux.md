# Editor UI/UX design (ROADMAP E8)

The design document for the editor redesign: what was wrong, the principles
the new editor follows, the design tokens, the component kit, the shell
layout and the shortcut table. Code lives under `src/editor/ui/` (design
system), `src/editor/shell/` (menu, toolbar, status bar, palette, layout,
commands) and `src/editor/panels/` (panels rebuilt on the kit).

## Capturing the editor

`scripts/editor-screenshots.sh [game] [out-dir] [prefix]` runs an
`ENGINE_EDITOR=ON` build under Xvfb + lavapipe at 1600x900 and writes one PNG
per state: `default`, `inspector` (an entity selected), `assets` (asset
browser in front) and `behaviour` (behaviour editor in front). The states
come from two startup hooks: `QUAT_EDITOR_SELECT=<entity name>` and
`QUAT_EDITOR_FOCUS=<panel title>`; `QUAT_WINDOW_SIZE=<w>x<h>` sets the window
size. `QUAT_SCREENSHOT` copies the presented swapchain image, so the capture
is the whole editor window with the UI drawn.

Before and after images: [`docs/screenshots/editor/`](screenshots/editor/).

**Why CI's editor capture looked wrong.** The capture itself works: the CI
`showcase-editor` image on the `ci-screenshots` branch does contain the docked
editor. It is useless for review because the window is 1280x720 and the
script halves every image of 1280 px or more, so the UI ends up as a
640x360 thumbnail of unreadable 8 px text. The remote `view.screenshot` path
(MCP) is different on purpose: in editor builds it copies the *viewport's*
offscreen image, never the UI, so a tool that asks the engine for a picture
of the editor gets only the scene. Editor captures therefore use `QUAT_SCREENSHOT` at
1600x900 without downscaling (`scripts/editor-screenshots.sh`). The CI
screenshot job (`scripts/ci-screenshots.sh`) captures `showcase-editor` and
`editor-inspector` the same way: `QUAT_WINDOW_SIZE=1600x900`,
`QUAT_EDITOR_LAYOUT=default`, no downscale.

## UX problems in the old editor

Found on the `before-*.png` captures (master at 1c90788):

| # | Problem | Where |
|---|---|---|
| 1 | Play is an ASCII `\|> Play` button floating in the middle of the menu bar; no pause or single step, no visible sim state. | menu bar |
| 2 | Gizmo modes are text pills (`move (W)`) drawn over the scene; no local/world space, no snapping, no camera speed. | viewport |
| 3 | Labels sit to the right of values (ImGui's default), so the eye reads numbers before knowing what they are; the label column is ragged. | inspector |
| 4 | Vector fields have no X/Y/Z cue; three identical grey boxes per row. | inspector |
| 5 | The inspector opens with `entity 11` (an internal id) and shows the name as a collapsible "component". | inspector |
| 6 | Component headers are bare collapsing headers: no icon, no enable toggle, removal hidden behind right-click, no reset / copy / paste. | inspector |
| 7 | "Add component" is an unsorted, unsearchable menu without descriptions. | inspector |
| 8 | Hierarchy rows are plain text: no type icons, no search, no visibility toggle, no rename; leaf and parent rows are misaligned. | hierarchy |
| 9 | Every dock tab bar carries ImGui's ▼ window-menu button and some carry close buttons; visual noise on every panel. | all panels |
| 10 | No status bar: frame time and entity counts live in a "Stats" panel that takes a quarter of the left column. | stats |
| 11 | The asset browser is a bare tree with `[mdl]`-style text tags; no search, breadcrumbs, grid view or type icons; drag-to-viewport is undiscoverable. | assets |
| 12 | No console: the log only goes to the terminal. | — |
| 13 | No command palette, no shortcut reference; shortcuts are discoverable only from button labels. | — |
| 14 | The File menu embeds a text field for the scene path; no Save As, no recent scenes, no shortcuts. | menu bar |
| 15 | One heavy font (Roboto Medium 16) for everything: no hierarchy between titles, labels and values; numbers are proportional. | everywhere |
| 16 | Panels, tab bars and headers use near-identical greys with no borders, so regions bleed together; the accent appears on a few random widgets. | everywhere |
| 17 | Spacing values (7x5 item spacing, 6x4 frame padding) are not on any scale. | everywhere |
| 18 | No light theme, no DPI awareness (fixed 16 px font regardless of display scale). | everywhere |
| 19 | The layout is not persisted (`IniFilename = nullptr`): every launch resets it. | shell |
| 20 | Empty states are a single line of disabled text ("(no entity selected)"). | inspector, behaviour |

How each is addressed is listed under [Resolution](#resolution).

## Principles

1. **Content first.** The viewport is the largest region; chrome is quiet
   (bg0 bars, no decoration that does not carry information).
2. **One way to do a thing, discoverable three ways.** Every action is a
   registered command: the same id drives the menu item, the toolbar button,
   the palette entry and the shortcut, and each surface shows the shortcut.
3. **Labels before values.** Property rows read left to right: label column,
   value filling the rest, reset arrow when the value differs from default.
4. **Semantic colour only.** Panels draw with token roles (`tokens().error`),
   never raw RGB; the accent marks the primary action and the active state,
   nothing else.
5. **A 4 px grid.** Every padding, gap and size is a step of the spacing
   scale, multiplied by the DPI scale.
6. **Every empty panel says what to do next** (icon, one line, a hint).

## Tokens

`src/editor/ui/theme.hpp`. Colours are sRGB-authored and the UI is blended in
sRGB (gamma) space, as ImGui expects: `ImGuiPass` draws through a UNORM view
of the sRGB swapchain (or a UNORM scratch image copied over it), so a token
lands on screen as its exact bytes and antialiased text keeps its weight in
both themes (issue #92). Both themes are tested
against WCAG 2.x (tests/editor/ui_theme_test.cpp): text, secondary text,
accent text and status colours >= 4.5:1 on every surface they are drawn on;
disabled text and axis markers >= 3:1.

| Role | Dark | Light | Use |
|---|---|---|---|
| `bg0` | `#131417` | `#DDE0E5` | menu, toolbar, status bar, tab bars, empty dock space |
| `bg1` | `#1B1C20` | `#F3F4F6` | panel bodies |
| `bg2` | `#25272C` | `#FFFFFF` | input fields, section headers, popups |
| `bg3` | `#30333A` | `#E6E8EC` | hovered / raised |
| `text` | `#E4E6EA` | `#1C1F24` | primary text |
| `text_secondary` | `#A2A7B1` | `#4A505A` | property labels, metadata, hints |
| `text_disabled` | `#6E737D` | `#737983` | inactive controls |
| `accent` | `#2B6CD4` | `#2160C4` | primary action, active toggle fill |
| `accent_text` | `#6EA8FE` | `#1D57B3` | accent text / icons / check marks |
| `on_accent` | `#FFFFFF` | `#FFFFFF` | text on an accent fill |
| `border` / `separator` | `#2E3036` / `#2A2C31` | `#C9CDD3` / `#D5D8DD` | outlines / hairlines |
| `success` `warning` `error` `info` | `#4CC38A` `#E8B04B` `#F26D66` `#62AEF5` | `#176B42` `#825200` `#B42A2A` `#1F63B5` | status |
| `selection` | accent @ 40 % | accent @ 22 % | selected rows |
| `axis_x/y/z` | `#E5484D` `#5DBB46` `#3D8CF0` | `#C8323A` `#3A8A1E` `#2466C8` | vector field markers |

**Spacing** (`ui/metrics.hpp`): unit 4 px; named steps `xxs 4`, `xs 8`,
`sm 12`, `md 16`, `lg 24`, `xl 32`. Rows are 24 px, the main toolbar 36 px,
the status bar 24 px. Radii: `sm 3` (checkbox, grab), `md 4` (frames,
buttons, tabs), `lg 6` (popups, floating windows, toasts).

**Type**: Inter Regular 14 px (UI), Inter SemiBold 14 px (headers, titles),
JetBrains Mono 13 px (console, numeric read-outs, code), Lucide icons merged
into each at 15 px (18 px for toolbar and empty-state icons).

**DPI**: `dpi_scale(display_scale, pixel_density)` from SDL3 gives the UI
scale (content scale, snapped to 1/8) and the font rasteriser density (pixel
density). Fonts are rebuilt and re-uploaded when either changes (moving the
window to another monitor); every size goes through `ui::scaled()`.

The window is created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so on a Retina
Mac (and a scaled Wayland output) the layout is in points and the framebuffer
in pixels (ImGui's `DisplayFramebufferScale` = 2); on Windows the window is
in pixels and only the content scale (150 %) scales the UI. The Viewport
panel renders the scene at its size in **pixels** (points x
`DisplayFramebufferScale`, `core::points_to_pixel_extent`), so the image is
drawn 1:1; mouse, picking, gizmo and overlay coordinates stay in points
relative to the image rect. The viewport stats overlay shows the scene's
`render` size (before the upscaler) and `output` size; the Stats panel,
Profiler and Renderer > Upscaling show both as "render -> output (scale)".

## Fonts

Vendored under `assets/fonts/` with their licences, loaded from the project
root at startup (`ui/fonts.cpp`); any missing file falls back to ImGui's
built-in font (icons then render as nothing rather than as tofu).

| File | Licence |
|---|---|
| `inter/Inter-Regular.ttf`, `inter/Inter-SemiBold.ttf` (Inter 4.1) | SIL OFL 1.1 (`inter/LICENSE.txt`) |
| `jetbrains-mono/JetBrainsMono-Regular.ttf` (2.304) | SIL OFL 1.1 (`jetbrains-mono/OFL.txt`) |
| `lucide/lucide.ttf` (lucide-static 1.82.0) | ISC, parts MIT (`lucide/LICENSE.txt`) |

`src/editor/ui/icons.hpp` is generated by `scripts/gen_editor_icons.py` from
`lucide/codepoints.json`; only listed icons are baked into the atlas.

Inter maps stylistic alternates into the Private Use Area where Lucide's
icons live, so the text faces are loaded with the PUA excluded
(`GlyphExcludeRanges`); without that, some icons render as Inter's
alternate digits and letters.

## Widget kit

`src/editor/ui/widgets.hpp` (implementation in `widgets_controls.cpp`,
`widgets_property.cpp`, `widgets_asset.cpp`, `toasts.cpp`). Panels build from
these and take colours and sizes from the tokens.

| Widget | Behaviour |
|---|---|
| `begin_property_grid` + `prop_*` | Two-column table. The label column is 40 % of the panel, clamped to 96-180 px (scaled). Labels are secondary text; a value that differs from its default turns its label primary and shows a reset arrow at the end of the row. Tooltips on labels; long labels ellipsise with the full name in the tooltip. |
| `prop_vec2/3/4` | One drag field per axis with a 3 px X/Y/Z colour stripe (tokens `axis_*`) and an axis tooltip. |
| `prop_color3/4` | Float colour editor (alpha bar and checkerboard preview for RGBA; HDR option). |
| `prop_enum` | Combo from a `{value, label, tooltip}` table; typed overload for enum classes. |
| `prop_asset` | Project-relative path field, drop target for files dragged from the asset browser, and a browse button listing matching project files with a filter. |
| `prop_text`, `prop_value`, `prop_note`, `prop_separator` | Text (optionally commit-on-Enter), read-only value (optionally monospace), a hint in the value column, a titled sub-group. |
| `component_header` | Full-width header: chevron, optional enable checkbox, icon, SemiBold title, and a ⋮ button (also right-click) with Reset to defaults / Copy values / Paste values / Remove component. |
| `section_header` | Plain collapsing section for panels such as Renderer and Stats. |
| `search_field` | Magnifier, hint, clear button, Esc clears; `matches_filter` is the shared case-insensitive match. |
| `icon_button`, `toggle_button`, `button` | Square frameless icon buttons (accent fill when toggled on, tooltip with shortcut, tooltips on disabled buttons too); text buttons in primary / secondary / danger / ghost. Icons are drawn centred on their ink box (`icon_origin` / `draw_icon`: the glyph's rasterised bounds, snapped to whole pixels), not passed as a button label, which ImGui centres by advance width and line height. |
| `begin_toolbar` / `toolbar_separator` | Tight uniform spacing and hairline separators. |
| `notify` / `draw_toasts` | Bottom-right toasts with a severity stripe and icon; auto-dismiss with a fade, click to dismiss, repeats restart instead of stacking; an optional action button (`ToastAction`) runs a follow-up and dismisses the toast. |
| `empty_state` | Centred large icon, title, wrapped hint (each line centred) and an optional primary action. |

## Shell

`src/editor/shell/`. `EditorLayer` (the public facade) owns one
`EditorShell`; `editor.cpp` went from 528 to about 160 lines.

```
+---------------------------------------------------------------------------+
| quat-3d  File Edit View Create Play Help                   scenes/x.json  |  menu bar
| > || >| [] | move rotate scale | World | snap 0.5 m | 1.0x | Search  Ctrl+P |  toolbar
+-----------+----------------------------------------+----------------------+
| Hierarchy | Viewport (no tab bar)                  | Inspector | Renderer |
|           |  [overlays][stats]           fps / ms  |                      |
+-----------+----------------------------------------+                      |
| Assets | Console | Animation | Behaviour | Physics | Stats | Profiler      |
+---------------------------------------------------------------------------+
| Editing | selection | 18 entities | newest log line ... | MCP | fps / ms   |  status bar
+---------------------------------------------------------------------------+
```

- **Commands** (`commands.hpp`): every action is registered once with id,
  label, icon, category, shortcut, scope, enabled / checked predicates and
  the action. Menus, toolbar, command palette and keyboard shortcuts all run
  through the registry. Shortcut scopes: *global* (not while typing unless
  the chord has Ctrl/Alt or is a function key), *viewport* (viewport
  hovered, and not while right-drag flying), *selection* (hierarchy or
  viewport focused).
- **Command palette** (Ctrl+P / Ctrl+Shift+P): fuzzy search (`fuzzy.cpp`:
  subsequence match scored for word starts, runs and early matches) over
  label + category; recently run commands first; disabled commands are
  listed dimmed with their reason as a tooltip.
- **Toolbar**: play controls coloured by state (Play green, Stop red while
  simulating; Pause / Step drive the A5 simulation clock), tool, space,
  snapping with per-tool increments, camera speed, and a search box that
  opens the palette.
- **Status bar**: Editing / Playing / Paused pill, selection, entity count,
  the newest log line (click opens the Console; bare third-party stderr lines
  below warning level are skipped), the remote endpoint ("MCP :7878  1
  client" or "Remote off"), frame rate and time in the monospace font.
- **Viewport**: the renderer already renders the scene into an offscreen
  image per frame slot (`FrameRenderer`, `ImGuiPass::viewport_texture_id`),
  so the viewport is a real dock panel sized to its content. Its overlay
  toolbar opens the overlays menu (colliders by motion type, navmesh,
  joints) and toggles the statistics overlay. Dropping a glTF places it; any
  other file is refused with a hint.
- **Layout**: `layout.cpp` builds the default dock tree; View > Reset Layout
  rebuilds it. ImGui's ini goes to `<project>/.quat/editor.ini` (git-ignored)
  with a `[QuatEditor][Prefs]` section for the theme, closed panels,
  snapping, viewport statistics, the current scene and recent scenes. The
  file is external data: `shell/prefs.cpp` parses each line on its own
  (numbers must be plain, finite decimals and are clamped to the UI's
  ranges; scene paths must be valid UTF-8, project-relative `.json` paths
  without `..`; recent scenes are de-duplicated and capped at eight) and
  ignores anything else. `QUAT_EDITOR_THEME` wins over the saved theme
  without replacing it: the ini keeps its theme until the user picks one in
  the editor.
- **Dialogs**: File > Open lists the project's scene files with a filter;
  Save As validates the path (project-relative, `.json`, warns before
  replacing); Help > Keyboard Shortcuts is generated from the registry;
  Edit > Preferences edits theme, camera speed and snap steps through the
  property grid; File > Build & Export (P4/E7) shows the game project and
  the package settings in the property grid, quat-pack's progress and log,
  and raises a toast with a "Show in folder" action when the package is
  written ("Show log" when the build fails).

## Shortcuts

Help > Keyboard Shortcuts generates this from the command registry:

| Action | Shortcut | Scope |
|---|---|---|
| New / Open / Save / Save As scene | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | global |
| Build & Export | Ctrl+B | global |
| Exit | Ctrl+Q | global |
| Undo / Redo (E1, disabled) | Ctrl+Z / Ctrl+Y | global |
| Rename / Duplicate / Delete | F2 / Ctrl+D / Delete | selection |
| Preferences | Ctrl+, | global |
| Command palette | Ctrl+P, Ctrl+Shift+P | global |
| Play / Pause / Step / Stop | F5 / F6 / F10 / Shift+F5 | global |
| Move / Rotate / Scale tool | W / E / R | viewport |
| Local / world space, snapping | X, Y (holding Ctrl inverts snapping while dragging) | viewport |
| Keyboard shortcuts | F1 | global |

Viewport navigation: right-drag looks, WASD / arrows fly, Q/E down/up, Shift
fast; click selects (empty space clears), Ctrl+click walks the selected agent
there, Shift+click adds a patrol waypoint.

## Panels

- **Hierarchy**: search (flat result list), type icons tinted by category
  (lights warm, cameras info, audio accent, AI success), inline rename (F2,
  double-click; Enter / focus loss commit, Esc cancels; an entity without
  a Name gets one), drag onto a row to
  re-parent and onto empty space to un-parent, keeping the entity's world
  pose (cycles refused by `Scene::set_parent`; a parent scaled to zero on
  an axis cannot hold the pose, so that drop is refused with a toast), a
  context menu with Create Child, Rename, Duplicate,
  Unparent and Delete, a + button with the Create menu, and an empty state
  with a Create Entity action.
- **Inspector**: entity header (type icon, name field, id), then one
  component header per component with codec-backed Reset / Copy / Paste
  (`scene::encode_components` / `apply_component`) and Remove. Reset writes
  the codec's defaults over the component in place, so what depends on it
  (a primitive's mesh, an instanced mesh's parts, scatter and placements)
  stays; components whose values come from an asset (animator, mesh) have
  no Reset. Remove goes through `scene::remove_component`, which also
  removes what the component implies, as the remote API does; fields in the
  property grid; a searchable Add Component popup built from
  `scene::component_infos()` with icons and one-line summaries. The existing
  `ComponentInspector<T>` specialisations keep their shape; optional members
  (`codec`, `icon`, `enabled()`, `property_grid`) opt them into the new
  header and grid, so specialisations added by other branches still compile
  and draw as before until migrated (E8.4). Procedural primitives have their
  own section now (shape, base colour, metallic, roughness, emissive).
- **Asset browser**: breadcrumbs, grid (tiles with tinted type icons and
  two-line names) and list (name / type / size) views, recursive search, drag
  any file (glTF onto the viewport places it), double-click to open a folder
  or place a model, a context menu with Place in Scene / Copy Path / Show in
  File Manager (`SDL_OpenURL`), and empty states for an empty folder, no
  results and no asset folders. Listings are cached and refreshed on
  navigation, search or every two seconds instead of every frame, and
  confined to the project: a folder path that is absolute, contains `..` or
  resolves through a symlink to outside the project lists nothing, and
  symlinked files or folders that resolve outside it are not listed.
- **Console**: tails the engine log ring (`core::LogRing`, A5)
  incrementally; error / warning / info / debug toggles with counts, text
  filter, auto-scroll (stops when you scroll up), copy visible lines, copy
  one line from its context menu, clear; monospace with token level colours.
  Editor builds tee stderr into the ring whenever the UI is up (previously
  only with `QUAT_REMOTE`).
- **Profiler** (R4, `panels/profiler.cpp`): the built-in frame profiler.
  Toolbar with freeze / reset toggles, kind buttons (All / Passes / Systems /
  Scopes / Waits), the frame budget and a search field; a property-grid
  summary of frame / CPU / GPU / wait time (avg, p95, max; monospace) with a
  warning note when GPU timing is off; a frame-time graph in token colours
  (wall time accent, GPU info, budget dashed in the warning colour); a
  sortable zone table with category-tinted kinds and values over the budget
  in the error colour; empty states for no profiler and no matching zones.

## Resolution

| # | Problem | Now |
|---|---|---|
| 1 | Floating ASCII Play button | Toolbar play / pause / step / stop with state colours, F5 / F6 / F10 / Shift+F5, sim-state pill in the status bar |
| 2 | Text gizmo pills | Toolbar tool buttons with icons + W/E/R, world/local (X), snapping with increments (Y), camera speed; viewport overlay for overlays and stats |
| 3 | Labels right of values | Property grid: label column left, value fills the row |
| 4 | No axis cues | X/Y/Z colour stripes on every vector field |
| 5 | `entity 11`, Name as a component | Entity header: icon, editable name, dim `#11` id |
| 6 | Bare component headers | Component header with icon, enable toggle, ⋮ menu (reset / copy / paste / remove) |
| 7 | Unsearchable Add Component | Searchable popup with icons and summaries from the codec |
| 8 | Plain hierarchy rows | Search, type icons, rename, duplicate, drag re-parent, context menu, empty state |
| 9 | ▼ menu and close buttons on every tab bar | Removed (`NoWindowMenuButton`, `NoCloseButton`); tool panels close from their tab or View |
| 10 | Stats eating the left column | Status bar + viewport statistics overlay; Stats is a bottom tab |
| 11 | Bare asset tree | Breadcrumbs, grid / list, search, type icons, drag hints, context menu |
| 12 | No console | Console panel on the log ring; newest line in the status bar |
| 13 | No palette or shortcut list | Command palette over every command; Help > Keyboard Shortcuts generated from the registry |
| 14 | Text field inside the File menu | New / Open / Save / Save As dialogs, recent scenes, shortcuts, toasts for results |
| 15 | One heavy font | Inter Regular / SemiBold, JetBrains Mono for numbers and logs, Lucide icons |
| 16 | Muddy greys | Token surfaces bg0-bg3 with borders; accent only for primary / active |
| 17 | Off-scale spacing | 4 px scale everywhere |
| 18 | No light theme, no DPI | Light theme (View > Theme, persisted), SDL3 display-scale aware fonts and metrics |
| 19 | Layout not persisted | `<project>/.quat/editor.ini`, View > Reset Layout |
| 20 | One-line empty states | `empty_state` in Inspector, Hierarchy, Assets, Console, Animation, Behaviour |

## Not done (E8.4 and later)

- **Hierarchy visibility toggle**: the engine has no per-entity visibility.
  An editor-only hide must live outside the ECS (CLAUDE.md: editor state is
  not stored in components) and every draw path (frame graph, shadows,
  instancing, grass, particles) would have to honour it, so it needs an
  engine-side "visible" concept first; it is renderer work, not a UI change.
- **Entity enable toggle** at the top of the Inspector: same reason, there is
  no entity-level enabled flag. Component toggles appear where a component
  has one (Behavior Tree).
- **Feature panels** still drawn with plain ImGui: Physics, the Animation
  preview (beyond its empty state), the behaviour editor (toolbar and node
  styling), the Grass and Audio inspector sections, and the upscaler /
  particles / scripts panels of the PRs still open. Already on the kit after
  the merge with master: the Renderer panel (sun plus every
  `RenderSettings` group of A8 / R12, with per-field reset arrows against
  the table defaults and a group reset), the Inspector's Material section
  (per-factor revert arrows, revert-all in its ⋮ menu) and the Animator /
  Foot IK sections (G8; Foot IK's enable toggle in its header).
- **Undo / redo** (E1): the commands exist, disabled.
- **Dynamic font atlas**: the renderer uploads the atlas once
  (`ImGuiBackendFlags_RendererHasTextures` is not implemented), so fonts are
  baked at fixed sizes and a DPI change rebuilds and re-uploads the atlas
  after a device idle. Implementing the 1.92 texture protocol in `ImGuiPass`
  would allow free font scaling and asset thumbnails (E6).
- **macOS shortcut labels** still read Ctrl.
