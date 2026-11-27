// R20 `.cube` look LUTs are untrusted input: the parser accepts the Adobe /
// Resolve text format, rejects everything else with the line and the reason,
// and never crashes on corrupted files.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "engine/asset/cube_lut.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace a = engine::asset;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Serialises `lut` as .cube text (red fastest).
std::string to_cube(const a::CubeLut& lut) {
    std::string s = "# test\nTITLE \"" + lut.title + "\"\nLUT_3D_SIZE " + std::to_string(lut.size) + "\n";
    for (std::size_t i = 0; i < lut.rgb.size(); i += 3) {
        s += std::to_string(lut.rgb[i]) + " " + std::to_string(lut.rgb[i + 1]) + " " +
             std::to_string(lut.rgb[i + 2]) + "\n";
    }
    return s;
}

TEST(CubeLut, ParsesTheAdobeFormat) {
    const std::string text =
        "# a comment\r\n"
        "TITLE \"Warm look\"\r\n"
        "\r\n"
        "LUT_3D_SIZE 2\r\n"
        "DOMAIN_MIN 0 0 0\r\n"
        "DOMAIN_MAX 1 1 1\r\n"
        "0 0 0\r\n1 0 0\r\n0 1 0\r\n1 1 0\r\n"
        "0 0 1\r\n1 0 1\r\n0 1 1\r\n1 1 1.0e0\r\n";
    auto lut = a::parse_cube_lut(text);
    ASSERT_OK(lut);
    EXPECT_EQ(lut->size, 2U);
    EXPECT_EQ(lut->title, "Warm look");
    ASSERT_EQ(lut->rgb.size(), 24U);
    // Red changes fastest, then green, then blue.
    EXPECT_EQ(lut->at(1, 0, 0), (std::array<float, 3>{1.0F, 0.0F, 0.0F}));
    EXPECT_EQ(lut->at(0, 1, 0), (std::array<float, 3>{0.0F, 1.0F, 0.0F}));
    EXPECT_EQ(lut->at(0, 0, 1), (std::array<float, 3>{0.0F, 0.0F, 1.0F}));
    EXPECT_EQ(lut->at(1, 1, 1), (std::array<float, 3>{1.0F, 1.0F, 1.0F}));

    // Resolve's input range form of the domain.
    const std::string resolve = "LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE -0.5 2\n"
                                "0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n";
    auto r = a::parse_cube_lut(resolve);
    ASSERT_OK(r);
    EXPECT_FLOAT_EQ(r->domain_min[1], -0.5F);
    EXPECT_FLOAT_EQ(r->domain_max[2], 2.0F);
}

TEST(CubeLut, IdentityRoundTripsThroughText) {
    const a::CubeLut id = a::identity_cube_lut(17);
    ASSERT_EQ(id.rgb.size(), 17U * 17U * 17U * 3U);
    EXPECT_FLOAT_EQ(id.at(16, 8, 0)[0], 1.0F);
    EXPECT_FLOAT_EQ(id.at(16, 8, 0)[1], 0.5F);
    auto parsed = a::parse_cube_lut(to_cube(id));
    ASSERT_OK(parsed);
    ASSERT_EQ(parsed->rgb.size(), id.rgb.size());
    for (std::size_t i = 0; i < id.rgb.size(); ++i) EXPECT_NEAR(parsed->rgb[i], id.rgb[i], 1e-6F);
}

TEST(CubeLut, RejectsMalformedFilesPrecisely) {
    const std::string body8 = "0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n";
    struct Case {
        std::string text;
        std::string expect;
    };
    const Case cases[] = {
        {"", "no LUT_3D_SIZE"},
        {"0 0 0\n", "data before LUT_3D_SIZE"},
        {"LUT_3D_SIZE 1\n0 0 0\n", "outside [2, 65]"},
        {"LUT_3D_SIZE 66\n", "outside [2, 65]"},
        {"LUT_3D_SIZE 99999\n", "LUT_3D_SIZE needs one integer"},
        {"LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n", "given twice"},
        {"LUT_1D_SIZE 1024\n", "1D LUTs are not supported"},
        {"LUT_3D_SIZE 2\n0 0 0\n", "1 entries, expected 8"},
        {"LUT_3D_SIZE 2\n" + body8 + "0 0 0\n", "more than 8 entries"},
        {"LUT_3D_SIZE 2\n0 0\n", "expected three numbers"},
        {"LUT_3D_SIZE 2\n0 0 nan\n", "not a finite number"},
        {"LUT_3D_SIZE 2\n0 0 1e99\n", "not a finite number"},
        {"LUT_3D_SIZE 2\n0 0 0x1p3\n", "not a finite number"},
        {"LUT_3D_SIZE 2\n0 0 1.5.5\n", "not a finite number"},
        {"LUT_3D_SIZE 2\n" + body8 + "TITLE \"late\"\n", "after data"},
        {"LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 1 1\n" + body8, "DOMAIN_MAX must exceed"},
        {"LUT_3D_SIZE 2\nDOMAIN_MIN 0 0\n", "needs three numbers"},
        {"LUT_3D_SIZE 2\nFOO 1\n", "unknown keyword 'FOO'"},
        {"TITLE no quotes\n", "quoted name"},
    };
    for (const Case& c : cases) {
        auto lut = a::parse_cube_lut(c.text);
        ASSERT_FALSE(lut.has_value()) << c.text;
        EXPECT_TRUE(contains(lut.error().message, c.expect))
            << c.text << " -> " << lut.error().message;
    }
    // Line numbers point at the offending line.
    auto bad = a::parse_cube_lut("# c\nLUT_3D_SIZE 2\n0 0 x\n");
    ASSERT_FALSE(bad.has_value());
    EXPECT_TRUE(contains(bad.error().message, "line 3")) << bad.error().message;
    // Oversized text is refused before parsing.
    const std::string huge(a::CubeLut::k_max_file_bytes + 1, '#');
    EXPECT_FALSE(a::parse_cube_lut(huge).has_value());
}

TEST(CubeLut, CorruptedFilesNeverCrash) {
    const std::string valid = to_cube(a::identity_cube_lut(5));
    std::mt19937 rng(20u);
    const std::string alphabet = "0123456789.-+eE \n\r#\"ABCDLUT_3DSIZEOMAINX\t";
    for (int round = 0; round < 2000; ++round) {
        std::string s = valid;
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < edits; ++k) {
            const std::size_t at = rng() % s.size();
            switch (rng() % 3) {
            case 0: s[at] = alphabet[rng() % alphabet.size()]; break;
            case 1: s.erase(at, 1 + rng() % 16); break;
            default: s.insert(at, std::string(1 + rng() % 4, alphabet[rng() % alphabet.size()])); break;
            }
            if (s.empty()) s = "x";
        }
        auto lut = a::parse_cube_lut(s);
        if (lut) {
            EXPECT_EQ(lut->rgb.size(), static_cast<std::size_t>(lut->size) * lut->size * lut->size * 3);
        }
    }
}

TEST(CubeLut, LoadsFromAFile) {
    const auto dir = std::filesystem::temp_directory_path() / "quat_cube_lut_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "id.cube";
    {
        std::ofstream out(path, std::ios::binary);
        out << to_cube(a::identity_cube_lut(3));
    }
    auto lut = a::load_cube_lut(path);
    ASSERT_OK(lut);
    EXPECT_EQ(lut->size, 3U);
    auto missing = a::load_cube_lut(dir / "missing.cube");
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(contains(missing.error().message, "missing.cube")) << missing.error().message;
    std::filesystem::remove_all(dir);
}

// The sample look shipped in assets/luts parses (grading.lut examples use it).
TEST(CubeLut, ShippedSampleLutParses) {
    auto lut = a::load_cube_lut(std::filesystem::path(QUAT_PROJECT_ROOT) / "assets" / "luts" /
                                "golden_hour.cube");
    ASSERT_OK(lut);
    EXPECT_EQ(lut->size, 17U);
    EXPECT_EQ(lut->title, "golden hour");
}

} // namespace
