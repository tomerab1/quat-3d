#pragma once

// Editor typography (ROADMAP E8.1): Inter for the UI, JetBrains Mono for
// code / log / numeric read-outs, and the Lucide icon font merged into each,
// loaded from <project root>/assets/fonts at the DPI scale's pixel sizes.
// Missing files degrade gracefully: the UI falls back to ImGui's built-in
// font and icon strings collapse to nothing (ic()), never to tofu boxes.
//
// The renderer's ImGuiPass uploads the atlas once (it does not implement
// ImGuiBackendFlags_RendererHasTextures), so fonts are baked at fixed sizes:
// always push them through push_font()/FontScope, which select the baked
// size. A DPI change rebuilds the atlas (EditorLayer) and the renderer
// re-uploads it.

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "metrics.hpp"

struct ImFont;
struct ImFontAtlas;

namespace engine::editor::ui {

enum class FontRole : std::uint8_t { body, heading, mono, icon_large };
inline constexpr std::size_t k_font_role_count = 4;

struct FontSet {
    std::array<ImFont*, k_font_role_count> fonts{};
    bool inter = false; // Inter loaded (else ImGui's default font)
    bool mono = false;  // JetBrains Mono loaded (else falls back to body)
    bool icons = false; // Lucide merged into every role
};

// Clears `atlas` and loads every role for `dpi` from `font_dir`. Does not
// build the atlas texture (the renderer does, on upload).
[[nodiscard]] FontSet load_fonts(ImFontAtlas& atlas, const std::filesystem::path& font_dir,
                                 DpiScale dpi);

// The fonts panels draw with (set by EditorLayer after load_fonts).
void set_fonts(const FontSet& set);
[[nodiscard]] const FontSet& fonts();
[[nodiscard]] ImFont* font(FontRole role);

// Pushes `role` at its baked size (ImGui::PushFont(font, LegacySize)).
void push_font(FontRole role);
void pop_font();
class FontScope {
public:
    explicit FontScope(FontRole role) { push_font(role); }
    ~FontScope() { pop_font(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

// `icon` when the icon font is loaded, else "" — for concatenating icons
// into labels without risking tofu.
[[nodiscard]] const char* ic(const char* icon);

// Glyph ranges (ImGui's zero-terminated [first, last] pair list) covering
// `codepoints`, merging consecutive code points into one range. Pure, for
// the icon table; the loader narrows the result to ImWchar.
[[nodiscard]] std::vector<std::uint32_t> glyph_ranges_for(std::span<const char32_t> codepoints);

} // namespace engine::editor::ui
