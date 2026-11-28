// Material shader source confinement (ROADMAP A6): text a remote client sends
// may not make slangc read files outside the shader library. Every file in
// tests/data/materials/unsafe/ is refused at the line its "expect-line"
// comment names; the safe test materials and the template pass. The
// dependency graph ignores string imports leaving the shader tree.

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/rhi/material_shader_source.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "support/material_shader_tree.hpp"

namespace {

namespace fs = std::filesystem;
using engine::rhi::check_material_source;

std::vector<std::string> library_modules() {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(quat::test::repo_shader_dir() / "lib")) {
        if (e.path().extension() == ".slang") out.push_back(e.path().stem().string());
    }
    return out;
}

fs::path data_dir() {
    return quat::test::repo_shader_dir().parent_path().parent_path() / "tests" / "data" /
           "materials";
}

TEST(MaterialShaderSource, RefusesEverythingThatReadsOtherFiles) {
    const std::vector<std::string> modules = library_modules();
    ASSERT_FALSE(modules.empty());
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(data_dir() / "unsafe")) {
        const std::string text = quat::test::read_text_file(entry.path());
        const std::size_t at = text.find("expect-line: ");
        ASSERT_NE(at, std::string::npos) << entry.path();
        const std::string line = text.substr(at + 13, text.find('\n', at) - at - 13);
        const auto r = check_material_source(text, modules);
        ASSERT_FALSE(r.has_value()) << entry.path();
        EXPECT_TRUE(r.error().message.starts_with("source line " + line + ":"))
            << entry.path() << ": " << r.error().message;
        ++checked;
    }
    EXPECT_GE(checked, 8);
}

// Sources may arrive with CRLF line ends (a Windows editor, a remote client):
// the checker reports the same lines and accepts the same files.
std::string with_crlf(const std::string& lf) {
    std::string out;
    out.reserve(lf.size() + lf.size() / 16);
    for (const char c : lf) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

TEST(MaterialShaderSource, CrlfSourcesReportTheSameLines) {
    const std::vector<std::string> modules = library_modules();
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(data_dir() / "unsafe")) {
        const std::string text = quat::test::read_text_file(entry.path());
        const auto lf = check_material_source(text, modules);
        const auto crlf = check_material_source(with_crlf(text), modules);
        ASSERT_FALSE(lf.has_value()) << entry.path();
        ASSERT_FALSE(crlf.has_value()) << entry.path() << " accepted with CRLF line ends";
        const auto line_of = [](const std::string& message) {
            return message.substr(0, message.find(':'));
        };
        EXPECT_EQ(line_of(crlf.error().message), line_of(lf.error().message))
            << entry.path() << ": " << crlf.error().message;
        ++checked;
    }
    EXPECT_GE(checked, 8);
    for (const fs::path& file :
         {data_dir() / "flat_glow.slang", data_dir() / "tricky_but_safe.slang",
          quat::test::repo_shader_dir() / "templates" / "material.slang"}) {
        const auto r = check_material_source(with_crlf(quat::test::read_text_file(file)), modules);
        EXPECT_TRUE(r.has_value()) << file << ": " << (r ? "" : r.error().message);
    }
}

TEST(MaterialShaderSource, AcceptsTheShippedAndTestMaterials) {
    const std::vector<std::string> modules = library_modules();
    for (const fs::path& file :
         {data_dir() / "flat_glow.slang", data_dir() / "tricky_but_safe.slang",
          data_dir() / "broken.slang", quat::test::repo_shader_dir() / "templates" / "material.slang",
          quat::test::repo_shader_dir() / "materials" / "lava.slang"}) {
        const auto r = check_material_source(quat::test::read_text_file(file), modules);
        EXPECT_TRUE(r.has_value()) << file << ": " << (r ? "" : r.error().message);
    }
}

TEST(MaterialShaderSource, UnknownModulesNameTheLibrary) {
    const auto r = check_material_source(
        quat::test::read_text_file(data_dir() / "unsafe" / "import_unknown.slang"),
        library_modules());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("material"), std::string::npos) << r.error().message;
}

// The message names what was refused.
std::string refusal(const char* file) {
    const auto r = check_material_source(
        quat::test::read_text_file(data_dir() / "unsafe" / file), library_modules());
    return r.has_value() ? std::string("accepted") : r.error().message;
}

TEST(MaterialShaderSource, RefusesCharacterLiteralsThatCouldOpenAFakeString) {
    // '"' would otherwise start a "string" that hides the import / include after it.
    for (const char* file :
         {"char_literal.slang", "char_quote_import.slang", "char_quote_include.slang"}) {
        const std::string why = refusal(file);
        EXPECT_NE(why.find("character literals"), std::string::npos) << file << ": " << why;
    }
}

TEST(MaterialShaderSource, RefusesLegacyAndReservedDoubleUnderscoreSpellings) {
    for (const char* file : {"legacy_import.slang", "legacy_import_module.slang"}) {
        const std::string why = refusal(file);
        EXPECT_NE(why.find("`__import`"), std::string::npos) << file << ": " << why;
    }
    EXPECT_NE(refusal("dunder_include.slang").find("`__include`"), std::string::npos);
}

// GPU safety (issue #110): a model-authored material must not be able to
// take the GPU down. Inline SPIR-V, pointers, raw buffer-device-address
// access and loops that obviously never end are refused with a message that
// names the construct.
TEST(MaterialShaderSource, RefusesInlineSpirvPointersAndRawAddresses) {
    const std::vector<std::pair<const char*, const char*>> cases{
        {"spirv_asm.slang", "inline SPIR-V (`spirv_asm`)"},
        {"pointer_cast.slang", "pointer dereference"},
        {"pointer_declaration.slang", "pointer types (`float4*`)"},
        {"pointer_user_type.slang", "pointer types (`Node*`)"},
        {"pointer_dereference.slang", "pointer dereference (`*p`)"},
        {"pointer_arrow.slang", "member access through a pointer (`->`)"},
        {"address_of.slang", "taking an address (`&x`)"},
        {"ptr_type.slang", "pointers (`Ptr<T>`)"},
        {"raw_buffer_load.slang", "`vk::` attributes and intrinsics"},
    };
    for (const auto& [file, what] : cases) {
        const std::string why = refusal(file);
        EXPECT_NE(why.find(what), std::string::npos) << file << ": " << why;
    }
}

TEST(MaterialShaderSource, RefusesLoopsThatNeverEnd) {
    for (const char* file : {"endless_for.slang", "endless_while.slang"}) {
        const std::string why = refusal(file);
        EXPECT_NE(why.find("loops need a bound known at compile time"), std::string::npos)
            << file << ": " << why;
    }
}

// `body` inside a material's evaluate().
std::string material_with(const std::string& helpers, const std::string& body) {
    return "import material;\n" + helpers +
           "\nstruct M : IMaterial {\n"
           "    static SurfaceSample evaluate(MaterialInput input) {\n"
           "        SurfaceSample s = input.standard;\n" +
           body +
           "\n        return s;\n    }\n}\n"
           "export struct SurfaceMaterial : IMaterial = M;\n";
}

TEST(MaterialShaderSource, PointerChecksLeaveArithmeticAlone) {
    const std::vector<std::string> modules = library_modules();
    // Multiplication, compound assignment, bitwise and logical and, postfix
    // increments, return-type arrows and bounded loops all look like the
    // refused constructs to a careless scan.
    const std::vector<std::pair<std::string, std::string>> ok{
        {"", "s.roughness = s.roughness * 0.5;"},
        {"", "s.roughness *= 0.5;"},
        {"", "float k = (s.metallic + 1.0) * -s.roughness * 2.0e-1;"},
        {"", "uint bits = asuint(s.roughness) & 0xffu; s.metallic = float(bits) * 0.01;"},
        {"", "if (s.alpha > 0.5 && s.metallic < 0.5) s.roughness = 0.2;"},
        {"", "int i = 0; float k = i++ * 2.0; s.roughness = k * s.albedo.x;"},
        {"", "float3 v = s.albedo; s.roughness = v.x * v.y * v[2];"},
        {"", "for (int i = 0; i < 4; ++i) { s.roughness *= 0.9; }"},
        {"", "[MaxIters(8)] while (s.roughness > 0.1) { s.roughness *= 0.5; }"},
        {"func half_of(x: float) -> float { return x * 0.5; }",
         "s.roughness = half_of(s.roughness) * half_of(1.0);"},
        {"struct Ramp { float a; }", "Ramp r = { 2.0 }; s.roughness = r.a * 0.25;"},
        {"", "s.roughness = sin(input.time) * cos(input.time) * (1.0 / 3.0);"},
    };
    for (const auto& [helpers, body] : ok) {
        const auto r = check_material_source(material_with(helpers, body), modules);
        EXPECT_TRUE(r.has_value()) << body << ": " << (r ? "" : r.error().message);
    }
    const std::vector<std::pair<std::string, std::string>> refused{
        {"", "float* p;"},
        {"", "float x = bit_cast<float*>(uint64_t(0))[0];"},
        {"", "vector<float, 3>* p;"},
        {"", "float x = 1.0; float y = x * *(&x);"},
        {"", "for (;;) { }"},
        {"", "do { } while (1);"},
        {"", "float x = ConstBufferPointer<float>::fromUInt(4096).get();"},
        {"", "[[vk::ext_instruction(1)]] float x;"},
    };
    for (const auto& [helpers, body] : refused) {
        EXPECT_FALSE(check_material_source(material_with(helpers, body), modules).has_value())
            << body;
    }
}

TEST(ShaderDependencies, IgnoresStringImportsLeavingTheTree) {
    using engine::rhi::ShaderDependencyGraph;
    for (const char* file : {"import_path.slang", "import_absolute.slang"}) {
        const auto imports = ShaderDependencyGraph::parse_imports(
            quat::test::read_text_file(data_dir() / "unsafe" / file));
        EXPECT_EQ(imports, std::vector<std::string>{"material"}) << file;
    }
}

} // namespace
