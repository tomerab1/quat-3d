// ShaderHotReload (R2), device-free: edits in a temp shader tree are picked up
// by the watcher, recompiled with slangc and reported per shader; a library
// module edit recompiles its importers; a failing module skips them. Runs
// without a GPU (the pipeline cache has no device, so no shader is "in use").
// Skips when slangc is not installed.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using namespace engine::rhi;
using State = ShaderStatus::State;

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string module_source(int factor) {
    return "module scale;\npublic uint scale_value(uint v) { return v * " +
           std::to_string(factor) + "; }\n";
}

std::string pass_source(int value) {
    return "import scale;\n"
           "[[vk::binding(0)]] RWStructuredBuffer<uint> g_out;\n"
           "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
           "void compute_main(uint3 tid : SV_DispatchThreadID) { g_out[tid.x] = scale_value(" +
           std::to_string(value) + "); }\n";
}

const ShaderStatus* find_status(const ShaderHotReload& r, const std::string& name) {
    for (const ShaderStatus& s : r.statuses()) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

class ShaderHotReloadTest : public ::testing::TestWithParam<bool> { // param: async
protected:
    void SetUp() override {
        if (!find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";
        fs::create_directories(lib_);
        fs::create_directories(out_);
        write_file(lib_ / "scale.slang", module_source(2));
        write_file(src_ / "pass.slang", pass_source(1));

        ShaderHotReload::Config config;
        config.source_dir = src_;
        config.output_dir = out_;
        config.debounce = std::chrono::milliseconds(0);
        config.poll_interval = std::chrono::milliseconds(0);
        config.async = GetParam();
        auto reload = ShaderHotReload::create(cache_, config);
        ASSERT_OK(reload);
        reload_ = std::move(*reload);
    }

    // Runs frames until every queued compile has been applied (or ~20 s).
    void settle() {
        // Let the watcher see the write first (inotify is immediate; polling
        // scans every update with a zero interval).
        for (int i = 0; i < 4000; ++i) {
            reload_->update(frame_++);
            if (i > 0 && reload_->idle()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ADD_FAILURE() << "hot reload did not settle";
    }

    quat::test::TempDir root_{"shader_hot_reload"};
    fs::path src_ = root_.path() / "shaders";
    fs::path lib_ = src_ / "lib";
    fs::path out_ = root_.path() / "cooked";
    PipelineCache cache_; // device-less: reload() finds no loaded shaders
    std::unique_ptr<ShaderHotReload> reload_;
    std::uint64_t frame_ = 0;
};

TEST_P(ShaderHotReloadTest, ListsSourcesAsCooked) {
    ASSERT_EQ(reload_->statuses().size(), 2U);
    EXPECT_EQ(reload_->statuses()[0].name, "lib/scale.slang");
    EXPECT_EQ(reload_->statuses()[1].name, "pass.slang");
    for (const ShaderStatus& s : reload_->statuses()) EXPECT_EQ(s.state, State::cooked);
    EXPECT_TRUE(reload_->idle());
}

TEST_P(ShaderHotReloadTest, RecompilesAnEditedPassAndReportsErrors) {
    write_file(src_ / "pass.slang", pass_source(5));
    settle();
    const ShaderStatus* pass = find_status(*reload_, "pass.slang");
    ASSERT_NE(pass, nullptr);
    EXPECT_EQ(pass->state, State::ok) << pass->message;
    EXPECT_EQ(pass->reloads, 1U);
    EXPECT_EQ(find_status(*reload_, "lib/scale.slang")->state, State::cooked);
    ASSERT_TRUE(fs::is_regular_file(out_ / "pass.spv"));
    const std::string good = read_file(out_ / "pass.spv");

    write_file(src_ / "pass.slang", "this is not slang;\n");
    settle();
    pass = find_status(*reload_, "pass.slang");
    EXPECT_EQ(pass->state, State::error);
    EXPECT_NE(pass->message.find("error"), std::string::npos) << pass->message;
    EXPECT_EQ(reload_->error_count(), 1U);
    EXPECT_EQ(read_file(out_ / "pass.spv"), good) << "a failed compile replaced the output";

    write_file(src_ / "pass.slang", pass_source(6));
    settle();
    pass = find_status(*reload_, "pass.slang");
    EXPECT_EQ(pass->state, State::ok) << pass->message;
    EXPECT_EQ(pass->reloads, 2U);
    EXPECT_EQ(reload_->error_count(), 0U);
}

TEST_P(ShaderHotReloadTest, ModuleEditRecompilesImporters) {
    write_file(lib_ / "scale.slang", module_source(3));
    settle();
    const ShaderStatus* mod = find_status(*reload_, "lib/scale.slang");
    const ShaderStatus* pass = find_status(*reload_, "pass.slang");
    EXPECT_EQ(mod->state, State::ok) << mod->message;
    EXPECT_EQ(pass->state, State::ok) << pass->message;
    EXPECT_EQ(mod->reloads, 1U);
    EXPECT_EQ(pass->reloads, 1U);
    EXPECT_TRUE(fs::is_regular_file(out_ / "scale.slang-module"));

    // A broken module fails, and its importer is reported without a compile.
    write_file(lib_ / "scale.slang", "module scale;\npublic uint scale_value(uint v) { oops }\n");
    settle();
    EXPECT_EQ(find_status(*reload_, "lib/scale.slang")->state, State::error);
    pass = find_status(*reload_, "pass.slang");
    EXPECT_EQ(pass->state, State::error);
    EXPECT_NE(pass->message.find("scale.slang failed to compile"), std::string::npos)
        << pass->message;
}

TEST_P(ShaderHotReloadTest, NewSourcesAreTrackedAndRequestRecompileWorks) {
    write_file(src_ / "extra.slang", pass_source(9));
    settle();
    const ShaderStatus* extra = find_status(*reload_, "extra.slang");
    ASSERT_NE(extra, nullptr);
    EXPECT_EQ(extra->state, State::ok) << extra->message;

    reload_->request_recompile(src_ / "pass.slang");
    settle();
    EXPECT_EQ(find_status(*reload_, "pass.slang")->reloads, 1U);
}

INSTANTIATE_TEST_SUITE_P(Modes, ShaderHotReloadTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("Async") : std::string("Sync");
                         });

TEST(ShaderHotReloadToggle, EnvironmentOverridesBuildDefault) {
    // The build default (ENGINE_EDITOR) applies when the variable is unset;
    // only the explicit values are checked here.
#if defined(_WIN32)
    _putenv_s("QUAT_SHADER_HOT_RELOAD", "0");
    EXPECT_FALSE(ShaderHotReload::enabled_by_environment());
    _putenv_s("QUAT_SHADER_HOT_RELOAD", "1");
    EXPECT_TRUE(ShaderHotReload::enabled_by_environment());
    _putenv_s("QUAT_SHADER_HOT_RELOAD", "");
#else
    setenv("QUAT_SHADER_HOT_RELOAD", "0", 1);
    EXPECT_FALSE(ShaderHotReload::enabled_by_environment());
    setenv("QUAT_SHADER_HOT_RELOAD", "1", 1);
    EXPECT_TRUE(ShaderHotReload::enabled_by_environment());
    unsetenv("QUAT_SHADER_HOT_RELOAD");
#endif
}

} // namespace
