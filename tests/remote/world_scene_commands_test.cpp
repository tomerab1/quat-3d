// scene.* and scene.batch on a real Scene through RpcServer::call.

#include <algorithm>
#include <string>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "remote/world_fixture.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;

class SceneCommands : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST_F(SceneCommands, CreateGetUpdateListDestroy) {
    const Json created = ok("scene.create",
                            {{"name", "crate"},
                             {"components",
                              {{"transform", {{"position", {1, 2, 3}}, {"scale", 2}}},
                               {"primitive", {{"shape", "cube"}, {"color", "#ff0000"}}}}}});
    ASSERT_TRUE(created.contains("id"));
    EXPECT_EQ(created["name"], "crate");
    const entt::entity e = entity(created["id"]);
    ASSERT_TRUE(reg().valid(e));
    EXPECT_TRUE(reg().all_of<s::Primitive>(e));
    // World transforms are current immediately (no tick needed).
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(e).world[3].y, 2.0F);

    const Json got = ok("scene.get", {{"entity", created["id"]}});
    EXPECT_EQ(got["name"], "crate");
    EXPECT_TRUE(got["parent"].is_null());
    EXPECT_TRUE(got["children"].empty());
    EXPECT_EQ(got["components"]["primitive"]["shape"], "cube");
    EXPECT_FLOAT_EQ(got["components"]["transform"]["position"][0].get<float>(), 1.0F);

    // Merge update: only the named fields change.
    ok("scene.update", {{"entity", "crate"},
                        {"name", "red crate"},
                        {"components", {{"primitive", {{"roughness", 0.9}}},
                                        {"point_light", {{"radius", 4}}}}}});
    EXPECT_EQ(reg().get<s::Name>(e).value, "red crate");
    EXPECT_FLOAT_EQ(reg().get<s::Primitive>(e).roughness, 0.9F);
    EXPECT_GT(reg().get<s::Primitive>(e).color.r, 0.99F); // kept
    EXPECT_FLOAT_EQ(reg().get<s::PointLight>(e).radius, 4.0F);

    const Json listed = ok("scene.list", {{"component", "point_light"}});
    ASSERT_EQ(listed["total"], 1);
    const Json& row = listed["entities"][0];
    EXPECT_EQ(row["id"], created["id"]);
    EXPECT_EQ(row["name"], "red crate");
    EXPECT_FLOAT_EQ(row["world_position"][2].get<float>(), 3.0F);
    const auto& comps = row["components"];
    EXPECT_NE(std::find(comps.begin(), comps.end(), "primitive"), comps.end());

    const Json removed = ok("scene.remove_components",
                            {{"entity", created["id"]}, {"components", {"point_light", "camera"}}});
    EXPECT_EQ(removed["removed"], Json::array({"point_light"}));
    EXPECT_FALSE(reg().all_of<s::PointLight>(e));
    err("scene.remove_components", {{"entity", created["id"]}, {"components", {"transform"}}},
        code::invalid_params);

    EXPECT_EQ(ok("scene.destroy", {{"entity", created["id"]}})["destroyed"], 1);
    EXPECT_FALSE(reg().valid(e));
    err("scene.get", {{"entity", created["id"]}}, code::not_found);
}

TEST_F(SceneCommands, ListFiltersAndPages) {
    for (int i = 0; i < 5; ++i) ok("scene.create", {{"name", "tree_" + std::to_string(i)}});
    ok("scene.create", {{"name", "rock"}});
    const Json trees = ok("scene.list", {{"name_contains", "tree"}, {"limit", 2}, {"offset", 1}});
    EXPECT_EQ(trees["total"], 5);
    ASSERT_EQ(trees["entities"].size(), 2U);
    EXPECT_EQ(trees["entities"][0]["name"], "tree_1"); // id order = creation order
    EXPECT_EQ(ok("scene.list")["total"], 6);
    EXPECT_TRUE(contains(err("scene.list", {{"component", "lights"}}, code::invalid_params),
                         "known:"));
    err("scene.list", {{"limit", 0}}, code::invalid_params);
}

TEST_F(SceneCommands, EntityByNameAndAmbiguity) {
    const Json a = ok("scene.create", {{"name", "lamp"}});
    ok("scene.create", {{"name", "lamp"}});
    ok("scene.create", {{"name", "desk"}});

    EXPECT_EQ(ok("scene.get", {{"entity", "desk"}})["name"], "desk");
    const std::string ambiguous = err("scene.get", {{"entity", "lamp"}}, code::invalid_params);
    EXPECT_TRUE(contains(ambiguous, "2 entities are named 'lamp'")) << ambiguous;
    EXPECT_TRUE(contains(ambiguous, std::to_string(a["id"].get<std::uint32_t>()))) << ambiguous;
    err("scene.get", {{"entity", "nobody"}}, code::not_found);
    err("scene.get", {{"entity", 123456}}, code::not_found);
    err("scene.get", {{"entity", true}}, code::invalid_params);
    err("scene.get", Json::object(), code::invalid_params);
    // Ids stay valid; a recycled slot gets a new id.
    ok("scene.destroy", {{"entity", a["id"]}});
    const Json again = ok("scene.create", {{"name", "lamp2"}});
    EXPECT_NE(again["id"], a["id"]);
    err("scene.get", {{"entity", a["id"]}}, code::not_found);
}

TEST_F(SceneCommands, StrictUnknownFieldsAndAtomicEdits) {
    std::string msg = err("scene.create", {{"name", "x"}, {"colour", "red"}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "colour: unknown field")) << msg;

    // Unknown component field: the error names the path and the known fields;
    // nothing is created.
    const std::size_t before = reg().view<s::Transform>().size();
    msg = err("scene.create",
              {{"name", "lamp"},
               {"components", {{"transform", {{"position", {0, 1, 0}}}},
                               {"point_light", {{"radius", 3}, {"brightness", 2}}}}}},
              code::invalid_params);
    EXPECT_TRUE(contains(msg, "components.point_light.brightness: unknown field")) << msg;
    EXPECT_TRUE(contains(msg, "intensity")) << msg;
    EXPECT_EQ(reg().view<s::Transform>().size(), before);

    msg = err("scene.create", {{"components", {{"sparkles", Json::object()}}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "unknown component 'sparkles'")) << msg;
    msg = err("scene.create", {{"components", {{"point_light", {{"radius", -1}}}}}},
              code::invalid_params);
    EXPECT_TRUE(contains(msg, "components.point_light.radius")) << msg;
    err("scene.create", {{"components", {{"mesh", Json::object()}}}}, code::invalid_params);
    EXPECT_EQ(reg().view<s::Transform>().size(), before);

    // A failing update leaves every component untouched, even ones listed
    // before the bad one.
    const Json box = ok("scene.create", {{"components", {{"primitive", {{"metallic", 0.25}}}}}});
    const entt::entity e = entity(box["id"]);
    const glm::mat4 local = reg().get<s::Transform>(e).local;
    err("scene.update",
        {{"entity", box["id"]},
         {"components", {{"primitive", {{"metallic", 1.0}}},
                         {"transform", {{"position", {9, 9, 9}}, {"scael", 2}}}}}},
        code::invalid_params);
    EXPECT_FLOAT_EQ(reg().get<s::Primitive>(e).metallic, 0.25F);
    EXPECT_EQ(reg().get<s::Transform>(e).local, local);
    // Animators can be edited but never added.
    err("scene.update", {{"entity", box["id"]}, {"components", {{"animator", {{"speed", 2}}}}}},
        code::invalid_params);
}

TEST_F(SceneCommands, SetParentKeepsWorldAndRefusesCycles) {
    const Json a = ok("scene.create",
                      {{"name", "a"}, {"components", {{"transform", {{"position", {10, 0, 0}}}}}}});
    const Json b = ok("scene.create",
                      {{"name", "b"}, {"components", {{"transform", {{"position", {12, 1, 0}}}}}}});
    ok("scene.set_parent", {{"entity", "b"}, {"parent", "a"}});
    const entt::entity eb = entity(b["id"]);
    EXPECT_EQ(reg().get<s::Parent>(eb).entity, entity(a["id"]));
    // keep_world (default): same world position, local now relative to a.
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(eb).world[3].x, 12.0F);
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(eb).local[3].x, 2.0F);
    EXPECT_EQ(ok("scene.get", {{"entity", "a"}})["children"], Json::array({b["id"]}));

    std::string msg = err("scene.set_parent", {{"entity", "a"}, {"parent", "b"}}, code::failed);
    EXPECT_TRUE(contains(msg, "cycle")) << msg;
    err("scene.set_parent", {{"entity", "a"}, {"parent", "a"}}, code::failed);
    err("scene.set_parent", {{"entity", "a"}}, code::invalid_params); // parent is required

    // keep_world = false keeps the local transform instead.
    ok("scene.set_parent", {{"entity", "b"}, {"parent", nullptr}, {"keep_world", false}});
    EXPECT_FALSE(reg().all_of<s::Parent>(eb));
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(eb).world[3].x, 2.0F);

    // Destroy is recursive.
    ok("scene.set_parent", {{"entity", "b"}, {"parent", "a"}});
    EXPECT_EQ(ok("scene.destroy", {{"entity", "a"}})["destroyed"], 2);
    EXPECT_FALSE(reg().valid(eb));
}

TEST_F(SceneCommands, CreateUnderParent) {
    const Json p = ok("scene.create",
                      {{"name", "table"}, {"components", {{"transform", {{"position", {0, 1, 0}}}}}}});
    const Json c = ok("scene.create",
                      {{"name", "cup"},
                       {"parent", "table"},
                       {"components", {{"transform", {{"position", {0, 0.5, 0}}}}}}});
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(entity(c["id"])).world[3].y, 1.5F);
    EXPECT_EQ(ok("scene.get", {{"entity", "cup"}})["parent"], p["id"]);
    err("scene.create", {{"name", "orphan"}, {"parent", "nowhere"}}, code::not_found);
}

TEST_F(SceneCommands, ClearKeepsLightsUnlessAsked) {
    const Json sun = ok("scene.create", {{"name", "sun"}, {"components", {{"dir_light", Json::object()}}}});
    const Json group = ok("scene.create", {{"name", "group"}, {"components", {{"transform", {{"position", {5, 0, 0}}}}}}});
    ok("scene.create", {{"name", "lamp"}, {"parent", "group"}, {"components", {{"point_light", Json::object()}}}});
    ok("scene.create", {{"name", "box"}, {"components", {{"primitive", Json::object()}}}});

    EXPECT_EQ(ok("scene.clear")["destroyed"], 2); // group + box
    EXPECT_TRUE(reg().valid(entity(sun["id"])));
    const Json lamp = ok("scene.get", {{"entity", "lamp"}});
    EXPECT_TRUE(lamp["parent"].is_null()); // detached, world placement kept
    EXPECT_FLOAT_EQ(lamp["components"]["transform"]["position"][0].get<float>(), 5.0F);

    EXPECT_EQ(ok("scene.clear", {{"keep_lights", false}})["destroyed"], 2);
    EXPECT_EQ(ok("scene.list")["total"], 0);
}

TEST_F(SceneCommands, ComponentsCatalogue) {
    const Json list = ok("scene.components");
    ASSERT_TRUE(list.is_array());
    bool saw_mesh = false;
    for (const Json& c : list) {
        if (c["name"] == "mesh") {
            saw_mesh = true;
            EXPECT_FALSE(c["writable"].get<bool>());
        }
    }
    EXPECT_TRUE(saw_mesh);
}

// R10.3: emitters are plain components — created, edited and checked over
// scene.create / scene.update like any other, with precise errors.
TEST_F(SceneCommands, ParticleEmittersAreScriptable) {
    const Json created = ok("scene.create",
                            {{"name", "campfire_sparks"},
                             {"components",
                              {{"transform", {{"position", {0, 0.2, 0}}}},
                               {"particle_emitter",
                                {{"rate", 150}, {"shape", "cone"}, {"blend", "additive"},
                                 {"emissive", 8}, {"collide", true}}}}}});
    const entt::entity e = entity(created["id"]);
    ASSERT_TRUE(reg().all_of<s::ParticleEmitter>(e));
    EXPECT_FLOAT_EQ(reg().get<s::ParticleEmitter>(e).settings.rate, 150.0F);
    ok("scene.update", {{"entity", "campfire_sparks"},
                        {"components", {{"particle_emitter", {{"max_particles", 4096}}}}}});
    EXPECT_EQ(reg().get<s::ParticleEmitter>(e).settings.max_particles, 4096U);
    EXPECT_TRUE(reg().get<s::ParticleEmitter>(e).settings.collide); // kept
    const std::string why =
        err("scene.update",
            {{"entity", "campfire_sparks"},
             {"components", {{"particle_emitter", {{"texture", "../../etc/passwd"}}}}}},
            code::invalid_params);
    EXPECT_NE(why.find("particle_emitter.texture"), std::string::npos) << why;
    const Json got = ok("scene.get", {{"entity", created["id"]}});
    EXPECT_EQ(got["components"]["particle_emitter"]["shape"], "cone");
}

TEST_F(SceneCommands, BatchWithBackReferences) {
    const Json result = ok(
        "scene.batch",
        {{"commands",
          {{{"method", "scene.create"}, {"params", {{"name", "house"}}}},
           {{"method", "scene.create"},
            {"params", {{"name", "roof"}, {"parent", "@0"},
                        {"components", {{"primitive", {{"shape", "cone"}}}}}}}},
           {{"method", "scene.update"},
            {"params", {{"entity", "@1"}, {"components", {{"transform", {{"position", {0, 3, 0}}}}}}}}},
           {{"method", "scene.get"}, {"params", {{"entity", "@0"}}}}}}});
    const Json& results = result["results"];
    ASSERT_EQ(results.size(), 4U);
    for (const Json& r : results) EXPECT_TRUE(r["ok"].get<bool>()) << r.dump();
    EXPECT_EQ(results[3]["result"]["children"], Json::array({results[1]["result"]["id"]}));
    EXPECT_FLOAT_EQ(reg().get<s::Transform>(entity(results[1]["result"]["id"])).world[3].y, 3.0F);
}

TEST_F(SceneCommands, BatchStopOnErrorAndBadReferences) {
    const Json commands = {
        {{"method", "scene.create"}, {"params", {{"name", "one"}}}},
        {{"method", "scene.get"}, {"params", {{"entity", "@5"}}}},           // forward ref
        {{"method", "scene.create"}, {"params", {{"name", "three"}}}},
        {{"method", "scene.update"}, {"params", {{"entity", "@1"}}}}};       // ref to a failure
    Json r = ok("scene.batch", {{"commands", commands}});
    ASSERT_EQ(r["results"].size(), 4U);
    EXPECT_TRUE(r["results"][0]["ok"].get<bool>());
    EXPECT_FALSE(r["results"][1]["ok"].get<bool>());
    EXPECT_EQ(r["results"][1]["error"]["code"], code::invalid_params);
    EXPECT_FALSE(r["results"][2]["ok"].get<bool>()); // skipped
    EXPECT_TRUE(contains(r["results"][2]["error"]["message"], "not run")) << r.dump();
    EXPECT_EQ(ok("scene.list", {{"name_contains", "three"}})["total"], 0);

    r = ok("scene.batch", {{"commands", commands}, {"stop_on_error", false}});
    EXPECT_TRUE(r["results"][2]["ok"].get<bool>());
    EXPECT_FALSE(r["results"][3]["ok"].get<bool>());
    EXPECT_TRUE(contains(r["results"][3]["error"]["message"], "failed")) << r.dump();

    // Structural problems reject the whole batch before anything runs.
    const std::size_t before = reg().view<s::Transform>().size();
    err("scene.batch",
        {{"commands", {{{"method", "scene.create"}}, {{"method", "scene.create"}, {"param", {}}}}}},
        code::invalid_params);
    err("scene.batch", {{"commands", {{{"method", "scene.batch"}}}}}, code::invalid_params);
    err("scene.batch", {{"commands", "scene.create"}}, code::invalid_params);
    EXPECT_EQ(reg().view<s::Transform>().size(), before);

    // Unknown methods fail per command.
    r = ok("scene.batch", {{"commands", {{{"method", "scene.explode"}}}}});
    EXPECT_EQ(r["results"][0]["error"]["code"], code::method_not_found);
    // "@N" outside a batch is just a (missing) name, with a hint.
    EXPECT_TRUE(contains(err("scene.get", {{"entity", "@0"}}, code::not_found), "scene.batch"));
}

} // namespace
