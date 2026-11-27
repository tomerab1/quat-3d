#pragma once

// Editor widget kit (ROADMAP E8.1). Small, reusable building blocks every
// panel draws with, so spacing, colour and behaviour stay consistent:
//
//   Text & tooltips   label(), text_secondary(), item_tooltip()
//   Buttons           icon_button(), toggle_button(), button() (primary /
//                     secondary / danger), toolbars
//   Search            search_field(): magnifier, hint, clear button, Esc clears
//   Headers           component_header(): collapsing, icon, optional enable
//                     toggle and a ⋮ menu (reset / copy / paste / remove);
//                     section_header() for plain panel sections
//   Property grid     begin_property_grid() + prop_*(): a fixed label column,
//                     the value filling the rest, a reset arrow when the
//                     value differs from its default, tooltips on the label
//   Empty states      empty_state(): icon, title, hint, optional action
//
// All sizes come from ui/metrics.hpp and all colours from ui/theme.hpp.
// Private editor header (src/editor only), so ImGui types are fine here.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <glm/glm.hpp>
#include <imgui.h>

#include "theme.hpp"

namespace engine::editor::ui {

// Drag-and-drop payload for a project file (UTF-8 absolute path, NUL
// terminated): the asset browser is the source; asset fields and the
// viewport (glTF instantiate) are targets.
inline constexpr const char* k_asset_payload = "QUAT_ASSET_PATH";

// ---- Text ------------------------------------------------------------------

// "<icon> <text>" when the icon font is loaded, else just `text`.
[[nodiscard]] std::string label(const char* icon, std::string_view text);
void text_secondary(std::string_view text);
void text_colored(Rgba color, std::string_view text);
// Tooltip for the last item, shown after the hover delay. `shortcut` (may
// be null) is drawn dimmed after the text.
void item_tooltip(const char* text, const char* shortcut = nullptr);

// ---- Icons -----------------------------------------------------------------

// Draw origin for the single-codepoint `glyph` in the current font and size
// so that its ink box (the rasterised glyph bounds, not its advance or line
// height) is centred on `centre`, snapped to whole pixels. `ink_min` /
// `ink_max` receive that ink box. Text without an icon glyph centres its
// CalcTextSize box instead.
[[nodiscard]] ImVec2 icon_origin(const char* glyph, ImVec2 centre, ImVec2* ink_min = nullptr,
                                 ImVec2* ink_max = nullptr);
// Draws `glyph` centred on its ink at `centre`.
void draw_icon(ImDrawList* dl, const char* glyph, ImVec2 centre, ImU32 col);

// ---- Buttons ---------------------------------------------------------------

enum class ButtonKind : std::uint8_t { secondary, primary, danger, ghost };
// Text button; `size` 0 = fit the label.
bool button(const char* label, ButtonKind kind = ButtonKind::secondary, ImVec2 size = {});
// Square, frameless icon button with a tooltip (and shortcut hint).
bool icon_button(const char* id, const char* icon, const char* tooltip,
                 const char* shortcut = nullptr, bool enabled = true);
// Icon button that shows `active` with an accent fill; returns true when
// clicked (the caller flips the state).
bool toggle_button(const char* id, const char* icon, bool active, const char* tooltip,
                   const char* shortcut = nullptr, bool enabled = true);

// Toolbars: a horizontal run of icon buttons with tight, uniform spacing.
void begin_toolbar();
void toolbar_separator();
void end_toolbar();

// ---- Search ----------------------------------------------------------------

// Search / filter field: magnifier icon, `hint` when empty, a clear button
// when not, Esc clears while focused. `width` <= 0 fills the line. Returns
// true when the text changed.
bool search_field(const char* id, std::string& text, const char* hint = "Search",
                  float width = 0.0F);

// Case-insensitive substring test used by every filter (empty filter matches).
[[nodiscard]] bool matches_filter(std::string_view text, std::string_view filter);

// ---- Headers ---------------------------------------------------------------

enum class HeaderAction : std::uint8_t { none, reset, copy, paste, remove };

struct ComponentHeaderSpec {
    const char* id = nullptr;    // stable ImGui id (the codec name)
    const char* icon = nullptr;  // icon:: constant
    const char* title = nullptr; // display name
    bool*       enabled = nullptr; // shows an enable checkbox when non-null
    bool        can_reset = true;
    bool        can_copy = true;
    bool        can_paste = false; // clipboard holds values for this component
    bool        removable = true;
    const char* tooltip = nullptr; // on the title (e.g. the codec summary)
    bool        default_open = true;
};
struct ComponentHeader {
    bool         open = false;
    HeaderAction action = HeaderAction::none;
};
// Full-width collapsing header for an inspector component. The ⋮ button and
// a right-click both open the menu. Does not push a tree level.
[[nodiscard]] ComponentHeader component_header(const ComponentHeaderSpec& spec);

// Collapsing section header for panels (Renderer, Stats): heading font, no
// menu. Returns open.
bool section_header(const char* label, bool default_open = true);

// ---- Empty states ----------------------------------------------------------

// Centred in the remaining region: a large icon, a title and a hint.
// Returns true when the optional `action` button is clicked.
bool empty_state(const char* icon, const char* title, const char* hint = nullptr,
                 const char* action = nullptr);

// ---- Property grid ---------------------------------------------------------

// A two-column table: labels left (fixed width from the panel width),
// values right. Every prop_* call is one row. Returns false when the grid is
// clipped away; call end_property_grid() only when it returned true.
bool begin_property_grid(const char* id);
void end_property_grid();

template <typename T>
struct PropOpts {
    std::optional<T> reset{};         // the default; enables the reset arrow
    const char*      tooltip = nullptr; // on the label
    bool             read_only = false;
};

struct DragSpec {
    float       speed = 0.01F;
    float       min = 0.0F; // min == max: unbounded
    float       max = 0.0F;
    const char* format = "%.3f";
    bool        slider = false; // a slider instead of a drag (needs min < max)
    bool        clamp = false;  // keep typed (Ctrl+click) values inside [min, max] too
};
struct IntSpec {
    float       speed = 1.0F;
    int         min = 0;
    int         max = 0;
    const char* format = "%d";
    bool        slider = false;
};

// Low-level row: draws the label cell (tooltip, modified marker) and moves
// to the value cell with the item width set to fill it, leaving room for the
// reset arrow when `has_reset`. Pair with prop_end().
void prop_begin(const char* label, const char* tooltip, bool has_reset, bool modified);
// Draws the reset arrow when `modified`; true when it was clicked.
bool prop_end(bool has_reset, bool modified);

bool prop_float(const char* label, float& v, const DragSpec& spec = {},
                const PropOpts<float>& opts = {});
// Edits an angle stored in radians, shown in degrees.
inline constexpr DragSpec k_degrees{.speed = 0.5F, .format = "%.1f°"};
bool prop_angle(const char* label, float& radians, const DragSpec& degrees_spec = k_degrees,
                const PropOpts<float>& opts = {});
bool prop_int(const char* label, int& v, const IntSpec& spec = {}, const PropOpts<int>& opts = {});
bool prop_u32(const char* label, std::uint32_t& v, const IntSpec& spec = {},
              const PropOpts<std::uint32_t>& opts = {});
bool prop_bool(const char* label, bool& v, const PropOpts<bool>& opts = {});
// X/Y/Z(/W) drag fields, each with its axis colour marker.
bool prop_vec2(const char* label, glm::vec2& v, const DragSpec& spec = {},
               const PropOpts<glm::vec2>& opts = {});
bool prop_vec3(const char* label, glm::vec3& v, const DragSpec& spec = {},
               const PropOpts<glm::vec3>& opts = {});
bool prop_vec4(const char* label, glm::vec4& v, const DragSpec& spec = {},
               const PropOpts<glm::vec4>& opts = {});
// A [min, max] pair (one drag with two handles).
bool prop_range(const char* label, glm::vec2& v, const DragSpec& spec = {},
                const PropOpts<glm::vec2>& opts = {});
// A full-width colour swatch showing its values; click opens a picker.
// `rgba` holds 3 floats (4 with `alpha`).
bool color_field(const char* id, float* rgba, bool alpha, bool hdr = false);
// Whether values are drawn on a swatch of `color` (as stored, clamped to
// 0..1) in dark text (true) or light text: the one with the higher WCAG
// contrast against the swatch's relative luminance.
[[nodiscard]] bool swatch_text_is_dark(Rgba color);
// Sliders drawn as a fill bar from the left edge to the value, with the value
// text on top: no grab handle for the text to collide with.
bool slider_float(const char* id, float* v, float min, float max, const char* format,
                  ImGuiSliderFlags flags = 0);
bool slider_int(const char* id, int* v, int min, int max, const char* format,
                ImGuiSliderFlags flags = 0);
bool prop_color3(const char* label, glm::vec3& v, const PropOpts<glm::vec3>& opts = {},
                 bool hdr = false);
bool prop_color4(const char* label, glm::vec4& v, const PropOpts<glm::vec4>& opts = {},
                 bool hdr = false);
// Single-line text. `commit_on_enter` applies only on Enter / focus loss.
bool prop_text(const char* label, std::string& v, const PropOpts<std::string>& opts = {},
               bool commit_on_enter = false);
// Read-only value (secondary text, monospace when `mono`).
void prop_value(const char* label, std::string_view value, const char* tooltip = nullptr,
                bool mono = false);

// Enums from a value table.
struct EnumItem {
    int         value = 0;
    const char* label = nullptr;
    const char* tooltip = nullptr;
};
bool prop_enum(const char* label, int& v, std::span<const EnumItem> items,
               const PropOpts<int>& opts = {});
template <typename E>
    requires std::is_enum_v<E>
bool prop_enum(const char* label, E& v, std::span<const EnumItem> items,
               const PropOpts<E>& opts = {}) {
    int raw = static_cast<int>(v);
    PropOpts<int> o;
    o.tooltip = opts.tooltip;
    o.read_only = opts.read_only;
    if (opts.reset) o.reset = static_cast<int>(*opts.reset);
    const bool changed = prop_enum(label, raw, items, o);
    if (changed) v = static_cast<E>(raw);
    return changed;
}

// A project-relative asset path: text field, a drop target for files dragged
// from the asset browser (k_asset_payload) and a browse button listing the
// project's files with one of `extensions`.
struct AssetSpec {
    std::filesystem::path             root{};       // project root (paths are relative to it)
    std::span<const std::string_view> extensions{}; // e.g. {".gltf", ".glb"}; empty = any
    bool                              commit_on_enter = true;
};
bool prop_asset(const char* label, std::string& path, const AssetSpec& spec,
                const PropOpts<std::string>& opts = {});

// A wrapped explanatory line in the value column (secondary text unless
// `color` is given): hints that would otherwise float outside the grid.
void prop_note(const char* text);
void prop_note(const char* text, Rgba color);

// Separator row spanning both columns (sub-group inside one component).
void prop_separator(const char* title = nullptr);

} // namespace engine::editor::ui
