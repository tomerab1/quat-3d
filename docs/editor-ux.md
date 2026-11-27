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
1600x900 without downscaling (`scripts/editor-screenshots.sh`).

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

`src/editor/ui/theme.hpp`. Colours are sRGB-authored. Both themes are tested
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
