// Runtime data-root resolution (packaged builds find assets/ + cooked/shaders/
// next to the executable; development builds fall back to the source tree).

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "engine/core/asset_root.hpp"
#include "engine/core/utf8_path.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace fs = std::filesystem;
using engine::core::AssetRootCandidates;
using engine::core::is_asset_root;
using engine::core::path_from_utf8;
using engine::core::path_to_generic_utf8;
using engine::core::path_to_utf8;
using engine::core::resolve_asset_root;

// "dir with spaces/ünïcødé" — spelled with escapes so the test does not depend
// on the compiler's source charset (MSVC without /utf-8).
const char8_t* const k_spaces_u8 = u8"dir with spaces";
const char8_t* const k_unicode_u8 = u8"\u00fcn\u00efc\u00f8d\u00e9";
const char* const k_unicode_utf8 = "\xC3\xBC" "n" "\xC3\xAF" "c" "\xC3\xB8" "d" "\xC3\xA9";
const char* const k_nested_utf8 = "dir with spaces/" "\xC3\xBC" "n" "\xC3\xAF" "c" "\xC3\xB8" "d" "\xC3\xA9";

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
    [[nodiscard]] fs::path make_root(const fs::path& name) const {
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

namespace {

// A packaged game unpacked under a non-ASCII directory with spaces (Windows:
// outside the ANSI code page's guarantees) is found, and the messages built
// from such paths are UTF-8 instead of aborting in a narrow conversion.
TEST(AssetRoot, ResolvesANonAsciiInstallDirWithSpaces) {
    TempDir tmp;
    const fs::path install = tmp.make_root(fs::path(k_spaces_u8) / fs::path(k_unicode_u8));

    AssetRootCandidates c;
    c.install_dirs = {install};
    const auto root = resolve_asset_root(c);
    ASSERT_OK(root);
    EXPECT_EQ(root->project_root, install);
    EXPECT_TRUE(fs::is_directory(root->shader_dir));

    c.override_root = install / "missing";
    const auto bad = resolve_asset_root(c);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find(path_to_utf8(install / "missing")), std::string::npos);
}

TEST(Utf8Path, RoundTripsNonAsciiPaths) {
    const fs::path p = path_from_utf8(k_nested_utf8);
    EXPECT_EQ(p, fs::path(k_spaces_u8) / fs::path(k_unicode_u8));
    EXPECT_EQ(path_to_generic_utf8(p), k_nested_utf8);
    EXPECT_EQ(path_to_utf8(p.filename()), k_unicode_utf8);
    EXPECT_EQ(path_from_utf8(k_unicode_utf8), fs::path(k_unicode_u8));
}

} // namespace
