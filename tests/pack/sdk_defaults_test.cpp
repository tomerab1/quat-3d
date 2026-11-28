// engine::pack::apply_installed_sdk_defaults: quat-pack installed in an engine
// SDK's bin/ finds the engine root and cooked shaders in share/quat3d
// (ROADMAP P8), without overriding defaults the build tree or the
// environment already set.

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "engine/pack/pack_cli.hpp"

namespace {

namespace fs = std::filesystem;
using engine::pack::apply_installed_sdk_defaults;
using engine::pack::PackDefaults;

class SdkDefaults : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        sdk_ = fs::temp_directory_path() / (std::string("quat-sdk-defaults-") + info->name());
        std::error_code ec;
        fs::remove_all(sdk_, ec);
        fs::create_directories(sdk_ / "bin");
        fs::create_directories(sdk_ / "share" / "quat3d" / "scripts");
        fs::create_directories(sdk_ / "share" / "quat3d" / "cooked" / "shaders");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(sdk_, ec);
    }
    void add_package_script() {
        std::ofstream(sdk_ / "share" / "quat3d" / "scripts" / "package-release.sh") << "#!/bin/sh\n";
    }
    fs::path sdk_;
};

TEST_F(SdkDefaults, InstalledLayoutProvidesEngineRootAndShaders) {
    add_package_script();
    PackDefaults d;
    d.tool_dir = sdk_ / "bin";
    apply_installed_sdk_defaults(d);
    ASSERT_TRUE(d.engine_root.has_value());
    ASSERT_TRUE(d.shader_dir.has_value());
    EXPECT_TRUE(fs::equivalent(*d.engine_root, sdk_ / "share" / "quat3d"));
    EXPECT_TRUE(fs::equivalent(*d.shader_dir, sdk_ / "share" / "quat3d" / "cooked" / "shaders"));
}

TEST_F(SdkDefaults, ExistingDefaultsWin) {
    add_package_script();
    PackDefaults d;
    d.tool_dir = sdk_ / "bin";
    d.engine_root = fs::path("/from/env");
    d.shader_dir = fs::path("/from/build/tree");
    apply_installed_sdk_defaults(d);
    EXPECT_EQ(*d.engine_root, fs::path("/from/env"));
    EXPECT_EQ(*d.shader_dir, fs::path("/from/build/tree"));
}

TEST_F(SdkDefaults, NoSdkLayoutLeavesDefaultsUnset) {
    // share/quat3d exists but has no package script: not an SDK.
    PackDefaults d;
    d.tool_dir = sdk_ / "bin";
    apply_installed_sdk_defaults(d);
    EXPECT_FALSE(d.engine_root.has_value());
    EXPECT_FALSE(d.shader_dir.has_value());

    PackDefaults no_tool_dir;
    apply_installed_sdk_defaults(no_tool_dir);
    EXPECT_FALSE(no_tool_dir.engine_root.has_value());
}

} // namespace
