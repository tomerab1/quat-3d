// Asset browser confinement for file symlinks (ROADMAP E8.4): a symlinked
// file whose target lies outside the project is neither listed nor found by
// the search, while one pointing inside the project still is.

#ifdef ENGINE_EDITOR

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "editor/asset_browser.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace ed = engine::editor;
namespace fs = std::filesystem;

void touch(const fs::path& p) {
    std::ofstream f(p, std::ios::binary);
    ASSERT_TRUE(f.good()) << p;
}

std::vector<std::string> names(const std::vector<ed::AssetBrowserState::Entry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) out.push_back(e.name);
    return out;
}

TEST(EditorAssetBrowser, FileSymlinksOutOfTheProjectAreNotListed) {
    const quat::test::TempDir tmp("asset-links");
    const fs::path root = tmp.path() / "project";
    const fs::path outside = tmp.path() / "outside";
    std::error_code ec;
    fs::create_directories(root / "assets", ec);
    fs::create_directories(outside, ec);
    touch(root / "assets/rock.glb");
    touch(outside / "secret.glb");
    fs::create_symlink(outside / "secret.glb", root / "assets/leak.glb", ec);
    if (ec) GTEST_SKIP() << "symlinks unavailable here: " << ec.message();
    fs::create_symlink(root / "assets/rock.glb", root / "assets/rock_alias.glb", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_EQ(names(ed::list_assets(root, "assets", "")),
              (std::vector<std::string>{"rock.glb", "rock_alias.glb"}));
    EXPECT_EQ(names(ed::list_assets(root, "", "glb")),
              (std::vector<std::string>{"assets/rock.glb", "assets/rock_alias.glb"}))
        << "the search skips it too";
    EXPECT_TRUE(ed::list_assets(root, "", "leak").empty());
}

} // namespace

#endif // ENGINE_EDITOR
