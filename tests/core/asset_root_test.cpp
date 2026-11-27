// Runtime data-root resolution (packaged builds find assets/ + cooked/shaders/
// next to the executable; development builds fall back to the source tree).

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "engine/core/asset_root.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace fs = std::filesystem;
using engine::core::AssetRootCandidates;
using engine::core::is_asset_root;
using engine::core::resolve_asset_root;

// Scratch directory, removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("quat_asset_root_test_" + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

    // Creates <path>/<name> with the packaged layout.
    [[nodiscard]] fs::path make_root(const std::string& name) const {
        const fs::path root = path_ / name;
        fs::create_directories(root / "assets");
        fs::create_directories(root / "cooked" / "shaders");
        return root;
    }

private:
    fs::path path_;
};

TEST(AssetRoot, LayoutNeedsAssetsAndCookedShaders) {
    TempDir tmp;
    EXPECT_TRUE(is_asset_root(tmp.make_root("full")));

    const fs::path only_assets = tmp.path() / "only_assets";
    fs::create_directories(only_assets / "assets");
    EXPECT_FALSE(is_asset_root(only_assets));
    EXPECT_FALSE(is_asset_root(tmp.path() / "missing"));
    EXPECT_FALSE(is_asset_root(fs::path()));
}

TEST(AssetRoot, InstallDirWinsOverBuildTree) {
    TempDir tmp;
    const fs::path install = tmp.make_root("install");
    const fs::path tree = tmp.make_root("tree");

    AssetRootCandidates c;
    c.install_dirs = {tmp.path() / "not_here", install};
    c.build_tree_root = tree;
    const auto root = resolve_asset_root(c);
    ASSERT_OK(root);
    EXPECT_EQ(root->project_root, install);
    EXPECT_EQ(root->shader_dir, install / "cooked" / "shaders");
    EXPECT_EQ(root->origin, "install directory");
}

TEST(AssetRoot, BuildTreeFallbackUsesItsShaderDir) {
    TempDir tmp;
    const fs::path tree = tmp.path() / "tree";
    fs::create_directories(tree / "assets");
    const fs::path shaders = tmp.path() / "elsewhere" / "shaders";
    fs::create_directories(shaders);

    AssetRootCandidates c;
    c.install_dirs = {tmp.path() / "bin"}; // a build dir: no data next to the exe
    c.build_tree_root = tree;
    c.build_tree_shader_dir = shaders;
    const auto root = resolve_asset_root(c);
    ASSERT_OK(root);
    EXPECT_EQ(root->project_root, tree);
    EXPECT_EQ(root->shader_dir, shaders);
    EXPECT_EQ(root->origin, "build tree");
}

TEST(AssetRoot, OverrideWinsAndMustBeValid) {
    TempDir tmp;
    const fs::path override_root = tmp.make_root("override");
    const fs::path install = tmp.make_root("install");

    AssetRootCandidates c;
    c.override_root = override_root;
    c.install_dirs = {install};
    const auto root = resolve_asset_root(c);
    ASSERT_OK(root);
    EXPECT_EQ(root->project_root, override_root);
    EXPECT_EQ(root->origin, "QUAT_ASSET_ROOT");

    // A wrong override is reported, not silently replaced by the install dir.
    c.override_root = tmp.path() / "typo";
    const auto bad = resolve_asset_root(c);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find("QUAT_ASSET_ROOT"), std::string::npos);
}

TEST(AssetRoot, NothingFoundListsWhatWasTried) {
    TempDir tmp;
    AssetRootCandidates c;
    c.install_dirs = {tmp.path() / "bin"};
    c.build_tree_root = tmp.path() / "tree";
    const auto r = resolve_asset_root(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find((tmp.path() / "bin").string()), std::string::npos);
    EXPECT_NE(r.error().message.find("build tree"), std::string::npos);
}

} // namespace
