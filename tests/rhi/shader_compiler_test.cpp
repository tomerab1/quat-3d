// Shader compiler service (R2): slangc runs as a child process with the CMake
// flags, a valid shader installs .spv + reflection, an invalid one returns the
// diagnostics and leaves the last good outputs alone; library modules compile
// to .slang-module. The dependency graph maps a module edit to its importers.
// Tests that run slangc skip when it is not installed.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_reflection.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using namespace engine::rhi;

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr const char* k_module = R"(
// lib module with no entry point
module scale;
public uint scale_value(uint v) { return v * 3; }
)";

constexpr const char* k_valid_pass = R"(
import scale;
[[vk::binding(0)]] RWStructuredBuffer<uint> g_out;
[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) { g_out[tid.x] = scale_value(7); }
)";

constexpr const char* k_invalid_pass = R"(
[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) { undefined_identifier = 7 }
)";

struct ShaderTree {
    quat::test::TempDir root{"shader_compiler"};
    fs::path src = root.path() / "shaders";
    fs::path lib = src / "lib";
    fs::path out = root.path() / "cooked";

    ShaderTree() {
        fs::create_directories(lib);
        write_file(lib / "scale.slang", k_module);
        write_file(src / "pass.slang", k_valid_pass);
    }
};

#define REQUIRE_SLANGC()                                                    \
    if (!find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)"

TEST(ShaderCompiler, ArgumentsMatchTheCMakeFlags) {
    ShaderTree t;
    ShaderCompiler compiler;
    {
        // Any existing file passes as slangc here (nothing is run); a file
        // of our own keeps the test portable (no /bin/sh on Windows).
        const fs::path fake_slangc = t.root.path() / "fake-slangc";
        write_file(fake_slangc, "");
        auto c = ShaderCompiler::create({fake_slangc, t.src, {}, t.out});
        ASSERT_OK(c);
        compiler = std::move(*c);
    }
    const fs::path stage = "/stage";
    const fs::path pass = t.src / "pass.slang";
    const std::vector<std::string> pass_args = compiler.arguments(pass, stage);
    const std::vector<std::string> want_pass = {
        pass.string(), "-I", t.lib.string(), "-target", "spirv", "-emit-spirv-directly",
        "-fvk-use-entrypoint-name", "-o", (stage / "pass.spv").string(), "-reflection-json",
        (stage / "pass.json").string()};
    EXPECT_EQ(pass_args, want_pass);
    EXPECT_EQ(compiler.kind_of(pass), ShaderSourceKind::pass);

    const fs::path mod = t.lib / "scale.slang";
    EXPECT_EQ(compiler.kind_of(mod), ShaderSourceKind::module);
    const std::vector<std::string> want_mod = {mod.string(), "-I", t.lib.string(), "-o",
                                               (stage / "scale.slang-module").string()};
    EXPECT_EQ(compiler.arguments(mod, stage), want_mod);
    EXPECT_EQ(compiler.outputs_for(mod),
              (std::vector<fs::path>{t.out / "scale.slang-module"}));
}

TEST(ShaderCompiler, CompilesAValidShader) {
    REQUIRE_SLANGC();
    ShaderTree t;
    auto compiler = ShaderCompiler::create({{}, t.src, {}, t.out});
    ASSERT_OK(compiler);

    const ShaderCompileResult r = compiler->compile(t.src / "pass.slang");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.outputs.size(), 2U);
    EXPECT_TRUE(fs::is_regular_file(t.out / "pass.spv"));
    auto reflection = parse_reflection_json(read_file(t.out / "pass.json"));
    ASSERT_OK(reflection);
    ASSERT_EQ(reflection->entry_points.size(), 1U);
    EXPECT_EQ(reflection->entry_points[0].name, "compute_main");
    ASSERT_EQ(reflection->descriptor_bindings.size(), 1U);
    EXPECT_EQ(reflection->descriptor_bindings[0].type, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
}

TEST(ShaderCompiler, CompilesALibraryModule) {
    REQUIRE_SLANGC();
    ShaderTree t;
    auto compiler = ShaderCompiler::create({{}, t.src, {}, t.out});
    ASSERT_OK(compiler);
    const ShaderCompileResult r = compiler->compile(t.lib / "scale.slang");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.kind, ShaderSourceKind::module);
    EXPECT_TRUE(fs::is_regular_file(t.out / "scale.slang-module"));
}

TEST(ShaderCompiler, ReturnsDiagnosticsAndKeepsTheLastGoodOutput) {
    REQUIRE_SLANGC();
    ShaderTree t;
    auto compiler = ShaderCompiler::create({{}, t.src, {}, t.out});
    ASSERT_OK(compiler);
    ASSERT_TRUE(compiler->compile(t.src / "pass.slang").ok);
    const std::string good_spv = read_file(t.out / "pass.spv");
    ASSERT_FALSE(good_spv.empty());

    write_file(t.src / "pass.slang", k_invalid_pass);
    const ShaderCompileResult r = compiler->compile(t.src / "pass.slang");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.outputs.empty());
    EXPECT_NE(r.diagnostics.find("error"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("undefined_identifier"), std::string::npos) << r.diagnostics;
    EXPECT_EQ(read_file(t.out / "pass.spv"), good_spv) << "failed compile clobbered the output";
}

// The .spv/.json pair is installed as a unit: when the second move fails
// (forced here by putting a non-empty directory where pass.spv goes), the
// reflection already moved into place is rolled back.
TEST(ShaderCompiler, FailedInstallRestoresThePreviousPair) {
    REQUIRE_SLANGC();
    ShaderTree t;
    auto compiler = ShaderCompiler::create({{}, t.src, {}, t.out});
    ASSERT_OK(compiler);
    ASSERT_TRUE(compiler->compile(t.src / "pass.slang").ok);
    const std::string good_json = read_file(t.out / "pass.json");
    ASSERT_FALSE(good_json.empty());

    std::error_code ec;
    fs::remove(t.out / "pass.spv", ec);
    fs::create_directories(t.out / "pass.spv");
    write_file(t.out / "pass.spv" / "blocker", "x");
    // A second binding: the new reflection differs from the installed one.
    write_file(t.src / "pass.slang", std::string("[[vk::binding(1)]] RWStructuredBuffer<uint> g_more;\n") +
                                         k_valid_pass);
    const ShaderCompileResult r = compiler->compile(t.src / "pass.slang");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.outputs.empty());
    EXPECT_NE(r.diagnostics.find("cannot install"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("previous outputs kept"), std::string::npos) << r.diagnostics;
    EXPECT_EQ(read_file(t.out / "pass.json"), good_json) << "reflection not rolled back";

    // Nothing is left behind in the staging area.
    for (const char* name : {"pass.spv", "pass.json"}) {
        EXPECT_FALSE(fs::exists(t.out / ".hot_reload" / name)) << name;
        EXPECT_FALSE(fs::exists(t.out / ".hot_reload" / "prev" / name)) << name;
    }

    // Unblocked, the same compile installs both.
    fs::remove_all(t.out / "pass.spv", ec);
    const ShaderCompileResult again = compiler->compile(t.src / "pass.slang");
    ASSERT_TRUE(again.ok) << again.diagnostics;
    EXPECT_NE(read_file(t.out / "pass.json"), good_json);
    EXPECT_TRUE(fs::is_regular_file(t.out / "pass.spv"));
}

TEST(ShaderCompiler, FailsWithoutSlangc) {
    ShaderTree t;
    EXPECT_FALSE(ShaderCompiler::create({"/nonexistent/slangc", t.src, {}, t.out}).has_value());
}

// --- dependency tracking ----------------------------------------------------

TEST(ShaderDependencies, ParsesImportDeclarations) {
    const auto imports = ShaderDependencyGraph::parse_imports(R"(
        import atmosphere;
        // import commented_out;
        /* import block_commented; */
        __exported import clustered_lights;
        import nested.module_name;
        import "quoted.slang";
        float important = 1.0; // 'import' inside an identifier
        import atmosphere; // duplicate
    )");
    EXPECT_EQ(imports, (std::vector<std::string>{"atmosphere", "clustered_lights",
                                                 "nested/module_name", "quoted.slang"}));
}

TEST(ShaderDependencies, ModuleEditRebuildsItsImporters) {
    const fs::path src = "/shaders";
    const fs::path lib = src / "lib";
    ShaderDependencyGraph graph(src, lib);
    graph.set_imports(lib / "common.slang", {});
    graph.set_imports(lib / "lighting_math.slang", {"common"}); // module -> module
    graph.set_imports(lib / "unrelated.slang", {});
    graph.set_imports(src / "lighting.slang", {"lighting_math"});
    graph.set_imports(src / "gbuffer.slang", {"common", "unrelated"});
    graph.set_imports(src / "tonemap.slang", {});

    const fs::path changed[] = {lib / "common.slang"};
    const std::vector<fs::path> order = graph.rebuild_order(changed);
    const std::vector<fs::path> want = {lib / "common.slang", lib / "lighting_math.slang",
                                        src / "gbuffer.slang", src / "lighting.slang"};
    EXPECT_EQ(order, want);

    // A pass edit rebuilds just that pass.
    const fs::path pass[] = {src / "tonemap.slang"};
    EXPECT_EQ(graph.rebuild_order(pass), (std::vector<fs::path>{src / "tonemap.slang"}));

    EXPECT_EQ(graph.imports_of(src / "gbuffer.slang"),
              (std::vector<fs::path>{lib / "common.slang", lib / "unrelated.slang"}));
}

TEST(ShaderDependencies, ScansTheRealShaderTree) {
    const fs::path src = fs::path(QUAT_COOKED_SHADER_DIR).parent_path().parent_path() /
                         "assets" / "shaders";
    if (!fs::is_directory(src)) GTEST_SKIP() << "no shader sources at " << src;
    ShaderDependencyGraph graph(src, src / "lib");
    graph.scan();
    const fs::path changed[] = {src / "lib" / "clustered_lights.slang"};
    const std::vector<fs::path> order = graph.rebuild_order(changed);
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.front(), (src / "lib" / "clustered_lights.slang").lexically_normal());
    const auto contains = [&](const char* name) {
        return std::find(order.begin(), order.end(), (src / name).lexically_normal()) !=
               order.end();
    };
    EXPECT_TRUE(contains("lighting.slang"));
    EXPECT_TRUE(contains("light_cull.slang"));
    EXPECT_TRUE(contains("transparent.slang"));
    EXPECT_FALSE(contains("tonemap.slang"));
}

// --- reflection compatibility -------------------------------------------------

ShaderReflection reflection(std::vector<DescriptorBinding> bindings, std::uint32_t push_bytes) {
    ShaderReflection r;
    r.entry_points.push_back({"compute_main", VK_SHADER_STAGE_COMPUTE_BIT});
    r.descriptor_bindings = std::move(bindings);
    if (push_bytes > 0) r.push_constant_ranges.push_back({0, push_bytes, VK_SHADER_STAGE_ALL});
    return r;
}

TEST(ShaderReflectionCompat, AcceptsBodyChangesAndRemovedBindings) {
    const std::string entries[] = {"compute_main"};
    const auto base = reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}},
                                 64);
    EXPECT_OK(check_reflection_compatible(base, base, entries));
    const auto fewer = reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}, 32);
    EXPECT_OK(check_reflection_compatible(base, fewer, entries));
}

TEST(ShaderReflectionCompat, RejectsLayoutChanges) {
    const std::string entries[] = {"compute_main"};
    const auto base = reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}, 64);
    EXPECT_ERR(check_reflection_compatible(
        base, reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                          {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}, 64),
        entries));                                                         // new binding
    EXPECT_ERR(check_reflection_compatible(
        base, reflection({{0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}, 64), entries)); // type
    EXPECT_ERR(check_reflection_compatible(
        base, reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}, 80), entries)); // push
    const std::string renamed[] = {"main"};
    EXPECT_ERR(check_reflection_compatible(base, base, renamed)); // entry point gone
}

// Passes bind by name (reflected_bindings), so a reload that moves another
// parameter of the same type into a slot must be refused.
TEST(ShaderReflectionCompat, RejectsSwappedParameterNames) {
    const std::string entries[] = {"compute_main"};
    const auto base = reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 0, "g_a"},
                                  {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 0, "g_b"}},
                                 0);
    EXPECT_OK(check_reflection_compatible(base, base, entries));
    const auto swapped = reflection({{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 0, "g_b"},
                                     {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 0, "g_a"}},
                                    0);
    const auto r = check_reflection_compatible(base, swapped, entries);
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("g_a"), std::string::npos) << r.error().message;
}

// --- untrusted reflection JSON (issue #6) ----------------------------------------

// Reflection files are read from disk (and rewritten by hot reload): malformed
// ones must be rejected with an error naming the field, never abort.
TEST(ShaderReflectionJson, RejectsMalformedInputWithoutAborting) {
    const char* bad[] = {
        R"({"entryPoints": [)",                                           // truncated
        R"([1, 2])",                                                      // not an object
        R"({"entryPoints": 5})",
        R"({"entryPoints": [5]})",
        R"({"entryPoints": [{"name": "main", "stage": 3}]})",
        R"({"entryPoints": [{"name": "main", "stage": "warp"}]})",
        R"({"entryPoints": [{"name": [], "stage": "compute"}]})",
        R"({"parameters": {}})",
        R"({"parameters": [7]})",
        R"({"parameters": [{"binding": 7}]})",
        R"({"parameters": [{"binding": {"kind": 5}}]})",
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot", "index": -1}}]})",
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot", "index": "0"}}]})",
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot", "space": 4294967296}}]})",
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot", "count": 0}}]})",
        R"({"parameters": [{"name": 7, "binding": {"kind": "descriptorTableSlot"}}]})",
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot"}, "type": 1}]})",
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot"},
             "type": {"kind": "resource", "baseShape": 7}}]})",
        R"({"parameters": [{"binding": {"kind": "pushConstantBuffer"},
             "type": {"elementType": {"fields": 3}}}]})",
        R"({"parameters": [{"binding": {"kind": "pushConstantBuffer"},
             "type": {"elementType": {"fields": [{"binding": {"kind": "uniform", "offset": "x"}}]}}}]})",
        R"({"parameters": [{"binding": {"kind": "pushConstantBuffer"},
             "type": {"elementType": {"fields": [{"binding": {"kind": "uniform", "offset": 0, "size": -4}}]}}}]})",
    };
    for (const char* text : bad) EXPECT_ERR(parse_reflection_json(text)) << text;

    const auto r = parse_reflection_json(
        R"({"parameters": [{"binding": {"kind": "descriptorTableSlot", "index": -1}}]})");
    ASSERT_ERR(r);
    EXPECT_NE(r.error().message.find("reflection.parameters[0].binding.index"), std::string::npos)
        << r.error().message;
}

TEST(ShaderReflectionJson, ParsesPushConstantsAndIgnoresUnknownFields) {
    const auto r = parse_reflection_json(R"({
        "entryPoints": [{"name": "main", "stage": "compute", "threadGroupSize": [8, 8, 1]}],
        "parameters": [
            {"name": "pc", "binding": {"kind": "pushConstantBuffer", "index": 0},
             "type": {"kind": "constantBuffer", "elementType": {"kind": "struct", "fields": [
                 {"name": "a", "binding": {"kind": "uniform", "offset": 0, "size": 16}},
                 {"name": "b", "binding": {"kind": "uniform", "offset": 16, "size": 8}}]}}},
            {"name": "g_tex", "binding": {"kind": "descriptorTableSlot", "index": 2, "count": 4},
             "type": {"kind": "resource", "baseShape": "texture2D", "access": "readWrite"}},
            {"name": "g_space", "binding": {"kind": "subElementRegisterSpace", "index": 1}}],
        "hashedStrings": {}})");
    ASSERT_OK(r);
    ASSERT_EQ(r->entry_points.size(), 1U);
    EXPECT_EQ(r->entry_points[0].stage, VK_SHADER_STAGE_COMPUTE_BIT);
    ASSERT_EQ(r->push_constant_ranges.size(), 1U);
    EXPECT_EQ(r->push_constant_ranges[0].size, 24U);
    ASSERT_EQ(r->descriptor_bindings.size(), 1U);
    EXPECT_EQ(r->descriptor_bindings[0].binding, 2U);
    EXPECT_EQ(r->descriptor_bindings[0].count, 4U);
    EXPECT_EQ(r->descriptor_bindings[0].type, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    EXPECT_EQ(r->descriptor_bindings[0].name, "g_tex");
}

// --- bindings by name -----------------------------------------------------------

TEST(ShaderReflectionBindings, ParsesNamesAndResolvesRequestsInOrder) {
    const auto parsed = parse_reflection_json(R"({"parameters": [
        {"name": "g_params", "binding": {"kind": "descriptorTableSlot", "index": 0},
         "type": {"kind": "constantBuffer"}},
        {"name": "g_out", "binding": {"kind": "descriptorTableSlot", "index": 3},
         "type": {"kind": "resource", "baseShape": "structuredBuffer", "access": "readWrite"}},
        {"name": "g_other", "binding": {"kind": "descriptorTableSlot", "index": 1, "space": 1},
         "type": {"kind": "resource", "baseShape": "structuredBuffer"}}]})");
    ASSERT_OK(parsed);
    ASSERT_NE(parsed->find_binding("g_out"), nullptr);
    EXPECT_EQ(parsed->find_binding("g_out")->binding, 3U);
    EXPECT_EQ(parsed->find_binding("g_missing"), nullptr);

    const BindingRequest ok[] = {{"g_out", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                 {"g_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}};
    const auto bindings = reflected_bindings(*parsed, ok, VK_SHADER_STAGE_COMPUTE_BIT);
    ASSERT_OK(bindings);
    ASSERT_EQ(bindings->size(), 2U);
    EXPECT_EQ((*bindings)[0].binding, 3U);
    EXPECT_EQ((*bindings)[0].stages, VK_SHADER_STAGE_COMPUTE_BIT);
    EXPECT_EQ((*bindings)[1].binding, 0U);

    const BindingRequest missing[] = {{"g_nope", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}};
    EXPECT_ERR(reflected_bindings(*parsed, missing, VK_SHADER_STAGE_COMPUTE_BIT));
    const BindingRequest wrong_type[] = {{"g_params", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}};
    EXPECT_ERR(reflected_bindings(*parsed, wrong_type, VK_SHADER_STAGE_COMPUTE_BIT));
    const BindingRequest other_set[] = {{"g_other", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}};
    EXPECT_ERR(reflected_bindings(*parsed, other_set, VK_SHADER_STAGE_COMPUTE_BIT));
}

} // namespace
