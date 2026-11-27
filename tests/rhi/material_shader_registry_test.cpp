// Material shader registry (ROADMAP A6): authored material shaders are created,
// edited and deleted by name; compiles go through the shader hot reload
// service and report slangc's diagnostics with line and column; a failed
// create leaves nothing behind and a failed update keeps the previous version
// (source and cooked program). Names are validated and confined to the
// materials directory. Tests that compile skip when slangc is absent.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "engine/rhi/material_shader_registry.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "support/expect_ok.hpp"
#include "support/material_shader_tree.hpp"

namespace {

namespace fs = std::filesystem;
using engine::rhi::MaterialShaderRegistry;
using engine::rhi::ShaderHotReload;
using Outcome = std::expected<MaterialShaderRegistry::Outcome, MaterialShaderRegistry::Error>;
using ErrorKind = MaterialShaderRegistry::ErrorKind;

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Registry + hot reload over a scratch material tree, driven frame by frame.
class MaterialShaderRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!engine::rhi::find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";
        ASSERT_TRUE(tree_.ready()) << tree_.error;
        auto reload = ShaderHotReload::create(cache_, tree_.reload_config());
        ASSERT_OK(reload);
        reload_ = std::move(*reload);
        registry_.emplace(tree_.registry_config(), reload_.get());
    }

    // Runs frames until `done` has been called; returns its outcome.
    Outcome run(bool create, const std::string& name, const std::string& source) {
        std::optional<Outcome> result;
        auto done = [&result](Outcome o) { result = std::move(o); };
        if (create) {
            registry_->create(name, source, done);
        } else {
            registry_->update(name, source, done);
        }
        settle([&] { return result.has_value(); });
        if (!result) return std::unexpected(MaterialShaderRegistry::Error{ErrorKind::failed, "no reply"});
        return std::move(*result);
    }

    template <typename Done>
    void settle(Done done) {
        for (int i = 0; i < 4000 && !done(); ++i) {
            reload_->update(frame_++);
            registry_->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Runs frames until every queued operation (incl. restores) has finished.
    void drain() {
        settle([&] { return registry_->pending() == 0 && reload_->idle(); });
    }

    quat::test::MaterialShaderTree         tree_{"material_registry"};
    engine::rhi::PipelineCache             cache_; // device-less: nothing is loaded
    std::unique_ptr<ShaderHotReload>       reload_;
    std::optional<MaterialShaderRegistry>  registry_;
    std::uint64_t                          frame_ = 1;
};

TEST_F(MaterialShaderRegistryTest, CreatesAValidShaderAndCooksItsProgram) {
    const Outcome r = run(true, "glow", quat::test::test_material("flat_glow.slang"));
    ASSERT_OK(r);
    EXPECT_TRUE(r->ok) << r->log;
    EXPECT_EQ(r->path, "assets/shaders/materials/glow.slang");
    EXPECT_TRUE(fs::is_regular_file(tree_.materials / "glow.slang"));
    for (const fs::path& p : registry_->cooked_paths("glow")) {
        EXPECT_TRUE(fs::is_regular_file(p)) << p;
    }

    const auto list = registry_->list();
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[0].name, "standard");
    EXPECT_TRUE(list[0].builtin);
    EXPECT_EQ(list[1].name, "glow");
    EXPECT_EQ(list[1].state, "ok");
    EXPECT_EQ(list[1].reloads, 1U);

    auto source = registry_->source("glow");
    ASSERT_OK(source);
    EXPECT_EQ(*source, quat::test::test_material("flat_glow.slang"));
}

TEST_F(MaterialShaderRegistryTest, FailedCreateReportsLocatedDiagnosticsAndLeavesNothing) {
    const Outcome r = run(true, "broken", quat::test::test_material("broken.slang"));
    ASSERT_OK(r);
    EXPECT_FALSE(r->ok);
    ASSERT_FALSE(r->diagnostics.empty()) << r->log;
    const engine::rhi::ShaderDiagnostic& d = r->diagnostics.front();
    EXPECT_EQ(d.severity, "error");
    EXPECT_EQ(d.file, "assets/shaders/materials/broken.slang") << "paths are project-relative";
    EXPECT_EQ(d.line, 9U);
    EXPECT_EQ(d.column, 33U);
    EXPECT_TRUE(contains(d.message, "uvv")) << d.message;
    EXPECT_FALSE(contains(r->log, tree_.project.generic_string())) << "absolute paths leaked";

    EXPECT_FALSE(fs::exists(tree_.materials / "broken.slang")) << "a failed create is undone";
    for (const fs::path& p : registry_->cooked_paths("broken")) EXPECT_FALSE(fs::exists(p)) << p;
    EXPECT_EQ(registry_->list().size(), 1U);
}

TEST_F(MaterialShaderRegistryTest, FailedUpdateKeepsThePreviousVersion) {
    const std::string good = quat::test::test_material("flat_glow.slang");
    ASSERT_TRUE(run(true, "glow", good).value_or(MaterialShaderRegistry::Outcome{}).ok);
    const fs::path spv = registry_->cooked_paths("glow").front();
    const std::string program = quat::test::read_text_file(spv);
    ASSERT_FALSE(program.empty());

    const Outcome r = run(false, "glow", quat::test::test_material("broken.slang"));
    ASSERT_OK(r);
    EXPECT_FALSE(r->ok);
    EXPECT_TRUE(r->kept_previous);
    EXPECT_EQ(r->diagnostics.front().line, 9U);
    EXPECT_EQ(quat::test::read_text_file(tree_.materials / "glow.slang"), good)
        << "the previous source is restored";
    EXPECT_EQ(quat::test::read_text_file(spv), program) << "the cooked program is untouched";

    drain(); // the restored source recompiles
    auto info = registry_->info("glow");
    ASSERT_OK(info);
    EXPECT_EQ(info->state, "ok") << info->message;

    // A good update replaces the program.
    const Outcome swapped = run(false, "glow", quat::test::test_material("flat_glow_swapped.slang"));
    ASSERT_OK(swapped);
    EXPECT_TRUE(swapped->ok) << swapped->log;
    EXPECT_NE(quat::test::read_text_file(spv), program);
}

TEST_F(MaterialShaderRegistryTest, MissingExportExplainsTheContract) {
    const Outcome r = run(true, "orphan", quat::test::test_material("missing_export.slang"));
    ASSERT_OK(r);
    EXPECT_FALSE(r->ok);
    bool hinted = false;
    for (const auto& d : r->diagnostics) {
        hinted = hinted || contains(d.message, "export struct SurfaceMaterial");
    }
    EXPECT_TRUE(hinted) << r->log;
}

TEST_F(MaterialShaderRegistryTest, OwnResourcesDoNotFitThePass) {
    const Outcome r = run(true, "own", quat::test::test_material("own_resource.slang"));
    ASSERT_OK(r);
    EXPECT_FALSE(r->ok) << "a material may not declare bindings the GBuffer pass lacks";
    bool explained = false;
    for (const auto& d : r->diagnostics) explained = explained || contains(d.message, "does not fit");
    EXPECT_TRUE(explained) << r->log;
    EXPECT_FALSE(fs::exists(tree_.materials / "own.slang"));
    for (const fs::path& p : registry_->cooked_paths("own")) EXPECT_FALSE(fs::exists(p)) << p;
}

TEST_F(MaterialShaderRegistryTest, NamesAreValidatedAndConfined) {
    const std::string src = quat::test::test_material("flat_glow.slang");
    for (const char* bad : {"../escape", "a/b", "Lava", "1st", "", "standard", "material",
                            "gbuffer_common", "with space", "dot.name"}) {
        const Outcome r = run(true, bad, src);
        ASSERT_FALSE(r.has_value()) << bad;
        EXPECT_EQ(r.error().kind, ErrorKind::invalid) << bad << ": " << r.error().message;
    }
    EXPECT_FALSE(run(true, std::string(49, 'a'), src).has_value());
    EXPECT_FALSE(fs::exists(tree_.project / "assets" / "shaders" / "escape.slang"));

    const Outcome empty = run(true, "empty", "");
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind, ErrorKind::invalid);

    write_file(tree_.materials / "taken.slang", src);
    const Outcome exists = run(true, "taken", src);
    ASSERT_FALSE(exists.has_value());
    EXPECT_EQ(exists.error().kind, ErrorKind::exists);

    const Outcome missing = run(false, "absent", src);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind, ErrorKind::not_found);

    EXPECT_EQ(registry_->remove("standard").error().kind, ErrorKind::invalid);
    EXPECT_EQ(registry_->remove("absent").error().kind, ErrorKind::not_found);
    EXPECT_EQ(registry_->remove("../x").error().kind, ErrorKind::invalid);
}

TEST_F(MaterialShaderRegistryTest, RemoveDeletesSourceAndPrograms) {
    ASSERT_TRUE(run(true, "glow", quat::test::test_material("flat_glow.slang"))
                    .value_or(MaterialShaderRegistry::Outcome{})
                    .ok);
    EXPECT_OK(registry_->remove("glow"));
    EXPECT_FALSE(fs::exists(tree_.materials / "glow.slang"));
    for (const fs::path& p : registry_->cooked_paths("glow")) EXPECT_FALSE(fs::exists(p)) << p;
    EXPECT_EQ(registry_->info("glow").error().kind, ErrorKind::not_found);
}

TEST(MaterialShaderRegistry, WithoutACompilerListsAndReadsButCannotCompile) {
    quat::test::MaterialShaderTree tree("material_registry_nocompiler");
    write_file(tree.materials / "lava.slang", quat::test::test_material("flat_glow.slang"));
    write_file(tree.materials / "Not_A_Name.slang", "ignored");
    MaterialShaderRegistry registry(tree.registry_config(), nullptr);
    EXPECT_FALSE(registry.can_compile());

    const auto list = registry.list();
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[0].state, "builtin");
    EXPECT_TRUE(contains(list[0].path, "lib/material.slang")) << list[0].path;
    EXPECT_EQ(list[1].name, "lava");
    EXPECT_EQ(list[1].state, "not_compiled") << "no cooked program and no compiler";

    auto builtin = registry.source("standard");
    ASSERT_OK(builtin);
    EXPECT_TRUE(contains(*builtin, "interface IMaterial"));
    auto tmpl = registry.template_source();
    ASSERT_OK(tmpl);
    EXPECT_TRUE(contains(*tmpl, "export struct SurfaceMaterial : IMaterial"));

    std::optional<Outcome> result;
    registry.create("new_one", *tmpl, [&](Outcome o) { result = std::move(o); });
    ASSERT_TRUE(result.has_value()) << "fails right away";
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().kind, ErrorKind::unavailable);
    EXPECT_FALSE(fs::exists(tree.materials / "new_one.slang"));
}

TEST(MaterialShaderRegistry, TheTemplateCompiles) {
    if (!engine::rhi::find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";
    quat::test::MaterialShaderTree tree("material_template");
    ASSERT_TRUE(tree.ready()) << tree.error;
    const fs::path source = tree.materials / "starter.slang";
    write_file(source, quat::test::read_text_file(quat::test::repo_shader_dir() / "templates" /
                                                  "material.slang"));
    auto compiler = engine::rhi::ShaderCompiler::create(
        {{}, tree.shaders, quat::test::repo_shader_dir() / "lib", tree.cooked, tree.materials});
    ASSERT_OK(compiler);
    EXPECT_EQ(compiler->kind_of(source), engine::rhi::ShaderSourceKind::material);
    const auto r = compiler->compile(source);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    ASSERT_EQ(r.outputs.size(), 2U);
    EXPECT_EQ(r.outputs[0].filename(), "starter.gbuffer.spv");
}

} // namespace
