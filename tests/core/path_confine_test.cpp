// Path confinement shared by game.json, scene references and remote
// requests (core::confine_to_root, core::relative_inside).

#include <filesystem>

#include <gtest/gtest.h>

#include "engine/core/path_confine.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

namespace fs = std::filesystem;
using engine::core::confine_to_root;
using engine::core::relative_inside;

TEST(PathConfine, ConfinesRelativePaths) {
    quat::test::TempProject root("quat_confine");
    auto ok = confine_to_root(root.path(), "a/./b/../c.json");
    ASSERT_OK(ok);
    EXPECT_EQ(*ok, root.path() / "a" / "c.json");
    EXPECT_ERR(confine_to_root(root.path(), ""));
    EXPECT_ERR(confine_to_root(root.path(), "."));
    EXPECT_ERR(confine_to_root(root.path(), "../x"));
    EXPECT_ERR(confine_to_root(root.path(), "/abs"));
}

TEST(PathConfine, RelativeInside) {
    const fs::path root = fs::absolute("project");
    EXPECT_EQ(relative_inside(root, root / "assets" / "a.glb"), fs::path("assets") / "a.glb");
    EXPECT_EQ(relative_inside(root, root / "assets" / ".." / "b.json"), fs::path("b.json"));
    EXPECT_TRUE(relative_inside(root, root).empty());
    EXPECT_TRUE(relative_inside(root, root / ".." / "elsewhere").empty());
    EXPECT_TRUE(relative_inside(root, fs::absolute("projectile") / "x").empty());
}

} // namespace
