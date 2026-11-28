// scene_references (ROADMAP P4): the files a scene file names, decoded
// through the component codec like load_scene.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/scene_references.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

using nlohmann::json;
using engine::scene::SceneRefKind;

const char* const k_scene = R"({
  "version": 2,
  "entities": [
    {"name": "Crate", "parent": -1,
     "mesh": {"mesh_key": "assets/meshes/crate.gltf#mesh0.prim0", "material_key": "",
              "gltf": "assets/meshes/crate.gltf"}},
    {"name": "Trees", "parent": -1,
     "instanced_mesh": {"gltf": "assets/meshes/tree.glb", "instances": []}},
    {"name": "Music", "parent": -1, "audio_source": {"clip": "assets/audio/theme.ogg", "loop": true}},
    {"name": "Player", "parent": -1, "script": {"path": "game/player.lua", "properties": {}}},
    {"name": "Box", "parent": -1, "primitive": {"shape": "cube"}, "future_component": {"x": 1}}
  ]
})";

TEST(SceneReferences, ReportsEveryFileReference) {
    auto refs = engine::scene::scene_references(json::parse(k_scene));
    ASSERT_OK(refs);
    ASSERT_EQ(refs->size(), 4U);
    EXPECT_EQ((*refs)[0].kind, SceneRefKind::model);
    EXPECT_EQ((*refs)[0].path, "assets/meshes/crate.gltf");
    EXPECT_EQ((*refs)[0].where, "entities[0] 'Crate' mesh.gltf");
    EXPECT_EQ((*refs)[1].kind, SceneRefKind::model);
    EXPECT_EQ((*refs)[1].path, "assets/meshes/tree.glb");
    EXPECT_EQ((*refs)[2].kind, SceneRefKind::audio);
    EXPECT_EQ((*refs)[2].path, "assets/audio/theme.ogg");
    EXPECT_EQ((*refs)[3].kind, SceneRefKind::script);
    EXPECT_EQ((*refs)[3].path, "game/player.lua");
}

TEST(SceneReferences, FailsLikeTheLoader) {
    // A component the loader would refuse fails here too, naming the entity.
    auto bad = engine::scene::scene_references(json::parse(
        R"({"entities": [{"name": "X", "instanced_mesh": {"gltf": "../outside.glb"}}]})"));
    ASSERT_ERR(bad);
    EXPECT_NE(bad.error().message.find("entities[0] 'X'"), std::string::npos) << bad.error().message;
    EXPECT_ERR(engine::scene::scene_references(json::parse(R"({"entities": [3]})")));
    EXPECT_ERR(engine::scene::scene_references(json::parse(R"({"entities": [{"mesh": {"gltf": 5}}]})")));
    EXPECT_ERR(engine::scene::scene_references(json::parse(R"({"version": 2})")));
}

TEST(SceneReferences, SceneDocuments) {
    EXPECT_TRUE(engine::scene::is_scene_document(json::parse(R"({"entities": []})")));
    EXPECT_FALSE(engine::scene::is_scene_document(json::parse(R"({"entities": {}})")));
    EXPECT_FALSE(engine::scene::is_scene_document(json::parse(R"([1, 2])")));
}

TEST(SceneReferences, Files) {
    quat::test::TempProject dir("quat_scene_refs");
    dir.write("s.json", k_scene);
    auto refs = engine::scene::scene_file_references(dir.path() / "s.json");
    ASSERT_OK(refs);
    EXPECT_EQ(refs->size(), 4U);
    dir.write("bad.json", "{");
    auto bad = engine::scene::scene_file_references(dir.path() / "bad.json");
    ASSERT_ERR(bad);
    EXPECT_NE(bad.error().message.find("bad.json"), std::string::npos);
    EXPECT_ERR(engine::scene::scene_file_references(dir.path() / "missing.json"));
}

} // namespace
