#include "fonts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <system_error>

#include <imgui.h>

#include "engine/core/utf8_path.hpp"
#include "icons.hpp"

namespace engine::editor::ui {

namespace {

namespace fs = std::filesystem;

FontSet g_fonts;

// Text glyphs: Latin-1 plus the punctuation, arrows and shapes the editor's
// strings use (… — • → ⋮ ● ▶ ×). Inter covers all of them.
constexpr ImWchar k_text_ranges[] = {
    0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
    0x0100, 0x017F, // Latin Extended-A
    0x2010, 0x205E, // General Punctuation
    0x2190, 0x21FF, // Arrows
    0x2200, 0x22FF, // Mathematical Operators
    0x25A0, 0x25FF, // Geometric Shapes
    0,
};

// Inter maps stylistic alternates into the Private Use Area, where Lucide's
// icons live; text faces must never answer for those code points.
constexpr ImWchar k_private_use[] = {0xE000, 0xF8FF, 0};

// The icon table's code points as ImGui glyph ranges (built once; ImGui
// keeps the pointer for the atlas' lifetime).
const ImWchar* icon_ranges() {
    static const std::vector<ImWchar> ranges = [] {
        std::vector<char32_t> cps;
        cps.reserve(icon::k_icon_count);
        for (const icon::IconEntry& e : icon::k_icons) cps.push_back(e.codepoint);
        std::vector<ImWchar> out;
        for (const std::uint32_t v : glyph_ranges_for(cps)) out.push_back(static_cast<ImWchar>(v));
        return out;
    }();
    return ranges.data();
}

[[nodiscard]] bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// Adds a TTF (owned by ImGui after the call) at `px` window pixels, or
// returns null when the file is missing / unreadable.
ImFont* add_ttf(ImFontAtlas& atlas, const fs::path& file, float px, DpiScale dpi,
                const ImWchar* ranges, bool merge, float min_advance = 0.0F,
                float offset_y = 0.0F) {
    if (!file_exists(file)) return nullptr;
    ImFontConfig cfg;
    cfg.MergeMode = merge;
    cfg.RasterizerDensity = dpi.raster;
    cfg.GlyphMinAdvanceX = min_advance;
    cfg.GlyphOffset = ImVec2(0.0F, offset_y);
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = merge; // icons on whole pixels; text keeps sub-pixel advances
    if (!merge) cfg.GlyphExcludeRanges = k_private_use;
    const std::string name = core::path_to_utf8(file.filename());
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s %.0fpx", name.c_str(), static_cast<double>(px));
    // UTF-8: ImGui opens it with the wide API on Windows (ImFileOpen).
    return atlas.AddFontFromFileTTF(core::path_to_utf8(file).c_str(), px, &cfg, ranges);
}

} // namespace

std::vector<std::uint32_t> glyph_ranges_for(std::span<const char32_t> codepoints) {
    std::vector<char32_t> sorted(codepoints.begin(), codepoints.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < sorted.size();) {
        std::size_t j = i;
        while (j + 1 < sorted.size() && sorted[j + 1] == sorted[j] + 1) ++j;
        out.push_back(static_cast<std::uint32_t>(sorted[i]));
        out.push_back(static_cast<std::uint32_t>(sorted[j]));
        i = j + 1;
    }
    out.push_back(0);
    return out;
}

FontSet load_fonts(ImFontAtlas& atlas, const fs::path& font_dir, DpiScale dpi) {
    atlas.Clear();
    FontSet set;
    const fs::path inter_regular = font_dir / "inter" / "Inter-Regular.ttf";
    const fs::path inter_semibold = font_dir / "inter" / "Inter-SemiBold.ttf";
    const fs::path mono_regular = font_dir / "jetbrains-mono" / "JetBrainsMono-Regular.ttf";
    const fs::path lucide = font_dir / "lucide" / "lucide.ttf";
    const bool have_icons = file_exists(lucide);

    // One text face + the icon font merged at a slightly larger size, icons
    // monospaced (min advance) and nudged down onto the text's optical centre.
    const auto add_role = [&](const fs::path& face, float logical_px) -> ImFont* {
        const float px = font_px(logical_px, dpi.ui);
        ImFont* f = add_ttf(atlas, face, px, dpi, k_text_ranges, false);
        if (f == nullptr) return nullptr;
        if (have_icons) {
            const float icon_px = font_px(TypeScale::icon * logical_px / TypeScale::body, dpi.ui);
            (void)add_ttf(atlas, lucide, icon_px, dpi, icon_ranges(), true, icon_px,
                          std::round(1.5F * dpi.ui));
        }
        return f;
    };

    ImFont* body = add_role(inter_regular, TypeScale::body);
    if (body == nullptr) {
        ImFontConfig cfg;
        cfg.SizePixels = font_px(13.0F, dpi.ui);
        cfg.RasterizerDensity = dpi.raster;
        body = atlas.AddFontDefault(&cfg);
        set.fonts = {body, body, body, body};
        std::fprintf(stderr, "[editor] UI fonts missing under %s; using the built-in font\n",
                     core::path_to_utf8(font_dir).c_str());
        set_fonts(set);
        return set;
    }
    set.inter = true;
    set.icons = have_icons;
    ImFont* heading = add_role(inter_semibold, TypeScale::heading);
    ImFont* mono = add_role(mono_regular, TypeScale::mono);
    set.mono = mono != nullptr;
    ImFont* large = nullptr;
    if (have_icons) {
        // Icon-only face for toolbar and empty-state glyphs; ASCII comes from
        // Inter so mixed labels still render.
        large = add_ttf(atlas, inter_regular, font_px(TypeScale::icon_large, dpi.ui), dpi,
                        k_text_ranges, false);
        const float icon_px = font_px(TypeScale::icon_large, dpi.ui);
        (void)add_ttf(atlas, lucide, icon_px, dpi, icon_ranges(), true, icon_px,
                      std::round(2.0F * dpi.ui));
    }
    set.fonts = {body, heading != nullptr ? heading : body, mono != nullptr ? mono : body,
                 large != nullptr ? large : body};
    set_fonts(set);
    return set;
}

void set_fonts(const FontSet& set) { g_fonts = set; }
const FontSet& fonts() { return g_fonts; }

ImFont* font(FontRole role) { return g_fonts.fonts[static_cast<std::size_t>(role)]; }

void push_font(FontRole role) {
    ImFont* f = font(role);
    ImGui::PushFont(f, f != nullptr ? f->LegacySize : 0.0F);
}

void pop_font() { ImGui::PopFont(); }

const char* ic(const char* icon) { return g_fonts.icons ? icon : ""; }

} // namespace engine::editor::ui
