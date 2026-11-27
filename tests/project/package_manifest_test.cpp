// manifest.json of a game package (ROADMAP P4): round trip, strict decode,
// version check.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/project/package_manifest.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace project = engine::project;

project::PackageManifest sample() {
    project::PackageManifest m;
    m.game_id = "dev.quat3d.test";
    m.game_name = "Test";
    m.game_version = "1.0.0";
    m.config = project::PackageConfig::development;
    m.target = "linux-x64";
    m.build_id = "dev.abc1234";
    m.engine = "quat-3d 0.1.0";
    m.files.push_back({"assets/scenes/a.json", 12, std::string(64, 'a'), project::FileKind::scene, false});
    m.files.push_back({"cooked/shaders/x.spv", 4, std::string(64, 'b'), project::FileKind::shader, false});
    m.files.push_back({"assets/meshes/m.glb", 9, std::string(64, 'c'), project::FileKind::model, true});
    return m;
}

TEST(PackageManifest, RoundTrip) {
    const nlohmann::json doc = project::manifest_json(sample());
    EXPECT_EQ(doc["format"], 1);
    EXPECT_EQ(doc["config"], "development");
    EXPECT_EQ(doc["game"]["id"], "dev.quat3d.test");
    auto back = project::parse_manifest(doc);
    ASSERT_OK(back);
    EXPECT_EQ(project::manifest_json(*back), doc);
    EXPECT_EQ(back->config, project::PackageConfig::development);
    ASSERT_EQ(back->files.size(), 3U);
    EXPECT_TRUE(back->files[2].cooked);
    EXPECT_EQ(back->files[1].kind, project::FileKind::shader);
}

TEST(PackageManifest, RejectsBadInput) {
    nlohmann::json doc = project::manifest_json(sample());
    doc["format"] = 2;
    auto e = project::parse_manifest(doc);
    ASSERT_ERR(e);
    EXPECT_NE(e.error().message.find("newer than this engine reads"), std::string::npos);

    doc = project::manifest_json(sample());
    doc["files"][0]["sha256"] = "XYZ";
    EXPECT_ERR(project::parse_manifest(doc));
    doc = project::manifest_json(sample());
    doc["files"][0]["kind"] = "video";
    EXPECT_ERR(project::parse_manifest(doc));
    doc = project::manifest_json(sample());
    doc["files"][0]["path"] = "../escape";
    EXPECT_ERR(project::parse_manifest(doc));
    doc = project::manifest_json(sample());
    doc["config"] = "release";
    EXPECT_ERR(project::parse_manifest(doc));
    doc = project::manifest_json(sample());
    doc["extra"] = 1;
    EXPECT_ERR(project::parse_manifest(doc));
    EXPECT_ERR(project::parse_manifest(nlohmann::json::array()));
}

TEST(PackageManifest, Configs) {
    EXPECT_EQ(*project::parse_package_config("shipping"), project::PackageConfig::shipping);
    EXPECT_EQ(*project::parse_package_config("development"), project::PackageConfig::development);
    EXPECT_ERR(project::parse_package_config("debug"));
    EXPECT_EQ(project::to_string(project::PackageConfig::shipping), "shipping");
}

} // namespace
