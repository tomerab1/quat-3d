// Editor icon table (ROADMAP E8.1): the generated header agrees with the
// vendored Lucide font's code points, every constant is the UTF-8 encoding
// of its code point, and the loader's glyph ranges cover exactly the table.

#ifdef ENGINE_EDITOR

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/ui/fonts.hpp"
#include "editor/ui/icons.hpp"

namespace {

namespace ui = engine::editor::ui;
namespace icon = engine::editor::icon;

// Decodes one UTF-8 sequence; 0 when malformed.
char32_t decode_utf8(std::string_view s, std::size_t& len) {
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    len = 0;
    if (s.empty()) return 0;
    if (b(0) < 0x80) {
        len = 1;
        return b(0);
    }
    if ((b(0) & 0xF0) == 0xE0 && s.size() >= 3) {
        len = 3;
        return (static_cast<char32_t>(b(0) & 0x0F) << 12) |
               (static_cast<char32_t>(b(1) & 0x3F) << 6) | static_cast<char32_t>(b(2) & 0x3F);
    }
    return 0;
}

TEST(EditorUiIcons, ConstantsEncodeTheirCodePoints) {
    ASSERT_GT(icon::k_icon_count, 50U);
    std::set<std::string_view> names;
    std::set<char32_t> codepoints;
    for (const icon::IconEntry& e : icon::k_icons) {
        std::size_t len = 0;
        const std::string_view utf8(e.utf8);
        EXPECT_EQ(decode_utf8(utf8, len), e.codepoint) << e.name;
        EXPECT_EQ(len, utf8.size()) << e.name << ": one code point per constant";
        // Lucide lives in the Basic Multilingual Plane's private use area, so
        // it fits ImGui's 16-bit ImWchar.
        EXPECT_GE(e.codepoint, 0xE000U) << e.name;
        EXPECT_LE(e.codepoint, 0xF8FFU) << e.name;
        EXPECT_TRUE(names.insert(e.name).second) << "duplicate name " << e.name;
        EXPECT_TRUE(codepoints.insert(e.codepoint).second) << "duplicate code point " << e.name;
    }
    EXPECT_TRUE(std::is_sorted(std::begin(icon::k_icons), std::end(icon::k_icons),
                               [](const icon::IconEntry& a, const icon::IconEntry& b) {
                                   return a.name < b.name;
                               }));
    EXPECT_STREQ(icon::play, "\xee\x84\xbc") << "spot check: play is U+E13C";
}

TEST(EditorUiIcons, TableMatchesTheVendoredFont) {
    const std::filesystem::path json_path =
        std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts/lucide/codepoints.json";
    std::ifstream in(json_path);
    ASSERT_TRUE(in) << "missing " << json_path;
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    ASSERT_TRUE(j.is_object()) << "codepoints.json does not parse";
    for (const icon::IconEntry& e : icon::k_icons) {
        const auto it = j.find(std::string(e.name));
        ASSERT_NE(it, j.end()) << e.name << " is not in the vendored font; regenerate icons.hpp";
        ASSERT_TRUE(it->is_number_unsigned()) << e.name;
        EXPECT_EQ(it->get<std::uint32_t>(), static_cast<std::uint32_t>(e.codepoint)) << e.name;
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(
        std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/fonts/lucide/lucide.ttf"));
}

TEST(EditorUiIcons, GlyphRangesMergeRunsAndTerminate) {
    const std::vector<char32_t> cps = {0xE005, 0xE001, 0xE002, 0xE003, 0xE003, 0xE010};
    const std::vector<std::uint32_t> r = ui::glyph_ranges_for(cps);
    const std::vector<std::uint32_t> want = {0xE001, 0xE003, 0xE005, 0xE005, 0xE010, 0xE010, 0};
    EXPECT_EQ(r, want);
    EXPECT_EQ(ui::glyph_ranges_for({}), std::vector<std::uint32_t>{0});
}

TEST(EditorUiIcons, GlyphRangesCoverEveryIcon) {
    std::vector<char32_t> cps;
    for (const icon::IconEntry& e : icon::k_icons) cps.push_back(e.codepoint);
    const std::vector<std::uint32_t> r = ui::glyph_ranges_for(cps);
    ASSERT_EQ(r.size() % 2, 1U);
    ASSERT_EQ(r.back(), 0U);
    std::size_t covered = 0;
    for (std::size_t i = 0; i + 1 < r.size(); i += 2) {
        ASSERT_LE(r[i], r[i + 1]);
        if (i >= 2) {
            EXPECT_GT(r[i], r[i - 1] + 1) << "adjacent ranges are merged";
        }
        covered += r[i + 1] - r[i] + 1;
    }
    EXPECT_EQ(covered, icon::k_icon_count) << "ranges cover the table and nothing else";
}

} // namespace

#endif // ENGINE_EDITOR
