// slangc diagnostics parsing (ROADMAP A6): header lines become located
// entries, the caret line under the echoed source gives the column, link
// errors without a location and notes without a code are kept.

#include <gtest/gtest.h>

#include "engine/rhi/shader_diagnostics.hpp"

namespace {

using engine::rhi::parse_slang_diagnostics;

TEST(ShaderDiagnostics, LocatesAnErrorByLineAndCaretColumn) {
    const auto ds = parse_slang_diagnostics(
        "/p/assets/shaders/materials/lava.slang(9): error 30027: 'uvv' is not a member of "
        "'MaterialInput'.\n"
        "        s.albedo = float3(input.uvv, 0.0);\n"
        "                                ^~~\n");
    ASSERT_EQ(ds.size(), 1U);
    EXPECT_EQ(ds[0].file, "/p/assets/shaders/materials/lava.slang");
    EXPECT_EQ(ds[0].line, 9U);
    EXPECT_EQ(ds[0].column, 33U);
    EXPECT_EQ(ds[0].severity, "error");
    EXPECT_EQ(ds[0].code, 30027U);
    EXPECT_EQ(ds[0].message, "'uvv' is not a member of 'MaterialInput'.");
    EXPECT_EQ(ds[0].source_line, "        s.albedo = float3(input.uvv, 0.0);");
}

TEST(ShaderDiagnostics, KeepsLinkErrorsNotesAndWarnings) {
    const auto ds = parse_slang_diagnostics(
        "(0): error 45001: unresolved external symbol '_SW15SurfaceMaterial'.\n"
        "(0): note: see declaration of 'SurfaceMaterial'\n"
        "a.slang(3): warning 41012: implicit conversion\r\n"
        "    float x = 1;\r\n"
        "          ^\r\n"
        "\n"
        "slangc exited with code 255\n");
    ASSERT_EQ(ds.size(), 3U);
    EXPECT_EQ(ds[0].file, "");
    EXPECT_EQ(ds[0].line, 0U);
    EXPECT_EQ(ds[0].code, 45001U);
    EXPECT_EQ(ds[0].column, 0U) << "the next line is another header, not source";
    EXPECT_EQ(ds[1].severity, "note");
    EXPECT_EQ(ds[1].code, 0U);
    EXPECT_EQ(ds[1].message, "see declaration of 'SurfaceMaterial'");
    EXPECT_EQ(ds[2].severity, "warning");
    EXPECT_EQ(ds[2].line, 3U);
    EXPECT_EQ(ds[2].column, 11U);
}

TEST(ShaderDiagnostics, IgnoresTextThatIsNotADiagnostic) {
    EXPECT_TRUE(parse_slang_diagnostics("").empty());
    EXPECT_TRUE(parse_slang_diagnostics("[gbuffer]\nall good\nf(x): not a header\n").empty());
    EXPECT_TRUE(parse_slang_diagnostics("file(12a): error 1: bad line number\n").empty());
    // A header whose following lines are not a caret pair gets no column.
    const auto ds = parse_slang_diagnostics("m.slang(2): error 1: x\nsome text\nmore text\n");
    ASSERT_EQ(ds.size(), 1U);
    EXPECT_EQ(ds[0].column, 0U);
    EXPECT_EQ(ds[0].source_line, "");
}

} // namespace
