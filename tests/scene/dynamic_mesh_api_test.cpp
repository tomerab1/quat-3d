#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/dynamic_mesh_api.hpp"
#include "support/expect_ok.hpp"

namespace s = engine::scene;
using nlohmann::json;

TEST(DynamicMeshApi, GenerateMeasuresOrSubmits) {
    s::DynamicMeshStore store;
    const json spec = {{"type", "stone"}, {"seed", 3}};
    auto measured = s::geometry_generate(&store, json{{"spec", spec}});
    ASSERT_OK(measured);
    EXPECT_GT((*measured)["triangles"].get<int>(), 100);
    EXPECT_EQ(store.stats().meshes, 0U);
    auto stored = s::geometry_generate(&store, json{{"spec", spec}, {"id", "rock"}});
    ASSERT_OK(stored);
    EXPECT_EQ((*stored)["id"], "rock");
    EXPECT_EQ((*stored)["version"], 1);
    EXPECT_EQ((*stored)["resident"], false);
    EXPECT_EQ((*stored)["hash"], (*measured)["hash"]);
    EXPECT_ERR(s::geometry_generate(&store, json{{"id", "x"}}));            // no spec
    EXPECT_ERR(s::geometry_generate(nullptr, json{{"spec", spec}, {"id", "x"}})); // no store
    EXPECT_ERR(s::geometry_generate(&store, json{{"spec", spec}, {"colour", 1}}));
}

TEST(DynamicMeshApi, PolygonOperations) {
    const json square = {{{"outer", {{0, 0}, {4, 0}, {4, 4}, {0, 4}}}}};
    auto shrunk = s::geometry_polygon(json{{"op", "offset"}, {"subject", square}, {"delta", -1.0}});
    ASSERT_OK(shrunk);
    EXPECT_NEAR((*shrunk)["area"].get<double>(), 4.0, 1e-6);
    auto cut = s::geometry_polygon(json{{"op", "subtract"},
                                        {"subject", square},
                                        {"clip", {{{"outer", {{1, 1}, {3, 1}, {3, 3}, {1, 3}}}}}}});
    ASSERT_OK(cut);
    EXPECT_NEAR((*cut)["area"].get<double>(), 12.0, 1e-6);
    EXPECT_EQ((*cut)["polygons"][0]["holes"].size(), 1U);
    auto fixed = s::geometry_polygon(json{{"op", "repair"}, {"subject", {{{"outer", {{0, 0}, {0, 2}, {2, 2}, {2, 0}}}}}}});
    ASSERT_OK(fixed);
    EXPECT_NEAR((*fixed)["area"].get<double>(), 4.0, 1e-9);
    EXPECT_ERR(s::geometry_polygon(json{{"op", "explode"}, {"subject", square}}));
    EXPECT_ERR(s::geometry_polygon(json{{"op", "offset"}, {"subject", {{{"outer", "x"}}}}}));
}

TEST(DynamicMeshApi, RawMeshesAndRegionUpdates) {
    s::DynamicMeshStore store;
    const json quad = {{"id", "quad"},
                       {"positions", {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
                       {"indices", {0, 2, 1, 0, 3, 2}}};
    auto set = s::dynamic_mesh_set(store, quad);
    ASSERT_OK(set);
    EXPECT_EQ((*set)["triangles"], 2);
    // Normals were computed (the quad faces +y), tangents too.
    const auto* mesh = store.mesh("quad");
    ASSERT_NE(mesh, nullptr);
    EXPECT_NEAR(mesh->vertices[0].normal.y, 1.0F, 1e-6F);
    EXPECT_NEAR(glm::length(glm::vec3(mesh->vertices[0].tangent)), 1.0F, 1e-5F);

    auto moved = s::dynamic_mesh_update(store, json{{"id", "quad"}, {"first", 2}, {"positions", {{1, 0.5, 1}}}});
    ASSERT_OK(moved);
    EXPECT_EQ((*moved)["version"], 2);
    EXPECT_FLOAT_EQ(store.mesh("quad")->vertices[2].position.y, 0.5F);
    EXPECT_ERR(s::dynamic_mesh_update(store, json{{"id", "quad"}, {"first", 3}, {"positions", {{0, 0, 0}, {0, 0, 0}}}}));

    auto bad = quad;
    bad["indices"] = {0, 1, 9};
    EXPECT_ERR(s::dynamic_mesh_set(store, bad));
    bad = quad;
    bad["indices"] = {0, 1};
    EXPECT_ERR(s::dynamic_mesh_set(store, bad));
    bad = quad;
    bad["normals"] = {{0, 1, 0}};
    EXPECT_ERR(s::dynamic_mesh_set(store, bad));

    auto list = s::dynamic_mesh_list(store, json::object());
    ASSERT_OK(list);
    EXPECT_EQ((*list)["meshes"].size(), 1U);
    EXPECT_EQ((*list)["stats"]["pending_uploads"], 1);
    EXPECT_OK(s::dynamic_mesh_get(store, json{{"id", "quad"}}));
    EXPECT_ERR(s::dynamic_mesh_get(store, json{{"id", "nope"}}));
    auto removed = s::dynamic_mesh_remove(store, json{{"id", "quad"}});
    ASSERT_OK(removed);
    EXPECT_EQ((*removed)["removed"], true);
}
