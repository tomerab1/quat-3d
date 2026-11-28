// render.settings.* and material.* (A8) against a real Scene and a
// RenderSettings standing in for the renderer's.

#include <expected>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/material_override.hpp"
#include "remote/world_fixture.hpp"
#include "support/temp_dir.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;
namespace r = engine::renderer;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class WorldTuning : public quat::test::WorldCommands {
protected:
    r::RenderSettings settings;
    void SetUp() override {
        services.render_settings = &settings;
        register_commands();
    }
};

class WorldTuningWithoutRenderer : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

TEST_F(WorldTuningWithoutRenderer, SettingsAreUnavailable) {
    err("render.settings.get", Json::object(), code::unavailable);
    err("render.settings.set", {{"bloom", {{"intensity", 0.1}}}}, code::unavailable);
    err("render.settings.reset", Json::object(), code::unavailable);
    // The schema is static: it works without a renderer.
    EXPECT_TRUE(ok("render.settings.schema").contains("groups"));
    // environment.get reports no clouds without the renderer's settings.
    EXPECT_TRUE(ok("environment.get")["clouds"].is_null());
}

TEST_F(WorldTuning, GetReturnsEveryGroupOrOne) {
    const Json all = ok("render.settings.get");
    for (const r::RenderSettingGroup& g : r::render_setting_groups()) {
        EXPECT_TRUE(all.contains(std::string(g.name))) << g.name;
    }
    EXPECT_EQ(all["exposure"]["auto"], true);
    EXPECT_FLOAT_EQ(all["bloom"]["intensity"].get<float>(), settings.bloom.intensity);

    const Json one = ok("render.settings.get", {{"group", "taa"}});
    EXPECT_EQ(one.size(), 1U);
    EXPECT_FLOAT_EQ(one["taa"]["feedback"].get<float>(), 0.9F);

    const std::string msg = err("render.settings.get", {{"group", "haze"}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "group: unknown group 'haze' (known: exposure")) << msg;
    err("render.settings.get", {{"groups", "taa"}}, code::invalid_params);
}

TEST_F(WorldTuning, SetMergesRangeChecksAndReturnsEverything) {
    const Json out =
        ok("render.settings.set",
           {{"exposure", {{"compensation_ev", 1.5}, {"auto", false}}}, {"shadows", {{"pcf_radius", 2}}}});
    EXPECT_FLOAT_EQ(settings.exposure.compensation_ev, 1.5F);
    EXPECT_FALSE(settings.exposure.auto_exposure);
    EXPECT_FLOAT_EQ(settings.shadows.pcf_radius, 2.0F);
    EXPECT_FLOAT_EQ(settings.exposure.key, 0.18F); // merged, not replaced
    EXPECT_EQ(out["shadows"]["pcf_radius"], 2);
    EXPECT_TRUE(out.contains("clouds"));

    std::string msg = err("render.settings.set", {{"bloom", {{"intensity", 4}}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "bloom.intensity: expected a number in [0, 1]")) << msg;
    msg = err("render.settings.set", {{"bloom", {{"glow", 1}}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "bloom.glow: unknown field (known: enabled, threshold")) << msg;
    // All or nothing.
    err("render.settings.set", {{"taa", {{"enabled", false}}}, {"ibl", {{"intensity", -1}}}},
        code::invalid_params);
    EXPECT_TRUE(settings.taa.enabled);
}

TEST_F(WorldTuning, ResetRestoresDefaults) {
    settings.bloom.intensity = 0.5F;
    settings.taa.feedback = 0.5F;
    Json out = ok("render.settings.reset", {{"group", "bloom"}});
    EXPECT_FLOAT_EQ(settings.bloom.intensity, r::BloomSettings{}.intensity);
    EXPECT_FLOAT_EQ(settings.taa.feedback, 0.5F);
    EXPECT_FLOAT_EQ(out["taa"]["feedback"].get<float>(), 0.5F);
    out = ok("render.settings.reset");
    EXPECT_FLOAT_EQ(settings.taa.feedback, r::TaaSettings{}.feedback);
    err("render.settings.reset", {{"group", "sky"}}, code::invalid_params);
}

TEST_F(WorldTuning, SchemaListsSettingsAndMaterialFields) {
    const Json schema = ok("render.settings.schema");
    ASSERT_TRUE(schema["groups"].is_array());
    EXPECT_EQ(schema["groups"][0]["name"], "exposure");
    ASSERT_TRUE(schema["material"].is_array());
    EXPECT_EQ(schema["material"].size(), s::material_fields().size());
    err("render.settings.schema", {{"verbose", true}}, code::invalid_params);
}

TEST_F(WorldTuning, EnvironmentCloudsShareTheRenderSettings) {
    ok("environment.set", {{"clouds", {{"coverage", 0.7}}}});
    EXPECT_FLOAT_EQ(settings.clouds.coverage, 0.7F);
    EXPECT_FLOAT_EQ(ok("render.settings.get", {{"group", "clouds"}})["clouds"]["coverage"].get<float>(),
                    0.7F);
    ok("render.settings.set", {{"clouds", {{"density", 40}}}});
    EXPECT_FLOAT_EQ(ok("environment.get")["clouds"]["density"].get<float>(), 40.0F);
}

// R21: a fake HDRI loader behind services.set_hdri / hdri_info.
class WorldHdri : public quat::test::WorldCommands {
protected:
    r::RenderSettings          settings;
    quat::test::TempDir        root{"hdri_root"};
    std::vector<std::string>   requested;
    engine::remote::HdriInfo   info;
    void SetUp() override {
        services.render_settings = &settings;
        services.project_root = [this] { return root.path(); };
        services.hdri_info = [this] { return info; };
        services.set_hdri = [this](const std::string& relative)
            -> std::expected<void, engine::core::Error> {
            requested.push_back(relative);
            if (relative == "hdri-cache/broken.hdr") {
                return std::unexpected(engine::core::Error{"hdr decode: corrupt"});
            }
            info.path = relative;
            info.loaded = !relative.empty();
            return {};
        };
        register_commands();
    }
};

// The sky source and the HDRI's look are the `environment` settings group;
// the file goes to services.set_hdri, confined to the project root, and a
// failed load (or a bad field) changes nothing.
TEST_F(WorldHdri, EnvironmentHdriSelectsAndLoads) {

    Json out = ok("environment.set", {{"sky", "hdri"},
                                      {"hdri", {{"path", "hdri-cache/sky.hdr"}, {"rotation_deg", 90},
                                                {"intensity", 2}, {"sun", false}}}});
    EXPECT_EQ(requested, std::vector<std::string>{"hdri-cache/sky.hdr"});
    EXPECT_EQ(settings.environment.source, static_cast<int>(r::SkySource::hdri));
    EXPECT_FLOAT_EQ(settings.environment.hdri_rotation_deg, 90.0F);
    EXPECT_FLOAT_EQ(settings.environment.hdri_intensity, 2.0F);
    EXPECT_FALSE(settings.environment.hdri_sun);
    EXPECT_EQ(out["sky"], "hdri");
    EXPECT_EQ(out["hdri"]["path"], "hdri-cache/sky.hdr");
    EXPECT_EQ(out["hdri"]["loaded"], true);
    EXPECT_FLOAT_EQ(out["hdri"]["rotation_deg"].get<float>(), 90.0F);
    EXPECT_FLOAT_EQ(
        ok("render.settings.get", {{"group", "environment"}})["environment"]["hdri_intensity"]
            .get<float>(),
        2.0F);

    // Escaping the project root, a bad field and a failed decode change nothing.
    std::string msg = err("environment.set", {{"sky", "procedural"}, {"hdri", {{"path", "../x.hdr"}}}},
                          code::invalid_params);
    EXPECT_TRUE(contains(msg, "escapes the project root")) << msg;
    err("environment.set", {{"hdri", {{"rotation_deg", 999}}}}, code::invalid_params);
    err("environment.set", {{"hdri", {{"exposure", 1}}}}, code::invalid_params);
    err("environment.set", {{"sky", "space"}}, code::invalid_params);
    msg = err("environment.set", {{"sky", "procedural"}, {"hdri", {{"path", "hdri-cache/broken.hdr"}}}},
              code::invalid_params);
    EXPECT_TRUE(contains(msg, "corrupt")) << msg;
    EXPECT_EQ(settings.environment.source, static_cast<int>(r::SkySource::hdri));
    EXPECT_FLOAT_EQ(settings.environment.hdri_rotation_deg, 90.0F);
    EXPECT_EQ(info.path, "hdri-cache/sky.hdr");

    // An empty path (or null) clears the file.
    ok("environment.set", {{"hdri", {{"path", nullptr}}}});
    EXPECT_EQ(requested.back(), "");
    EXPECT_TRUE(ok("environment.get")["hdri"]["path"].is_null());
}

TEST_F(WorldTuning, MaterialOfAPrimitive) {
    const Json created = ok("scene.create", {{"name", "ball"},
                                             {"components", {{"primitive", {{"shape", "sphere"}}}}}});
    Json m = ok("material.get", {{"entity", "ball"}});
    EXPECT_EQ(m["source"], "primitive");
    EXPECT_FLOAT_EQ(m["material"]["roughness"].get<float>(), 0.5F);
    EXPECT_TRUE(m["overridden"].empty());
    EXPECT_FALSE(m.contains("shared_by"));

    m = ok("material.set", {{"entity", created["id"]},
                            {"material", {{"roughness", 0.15}, {"clearcoat", 1}, {"base_color", "#ffffff"}}}});
    const auto& prim = reg().get<s::Primitive>(entity(created["id"]));
    EXPECT_FLOAT_EQ(prim.roughness, 0.15F);       // the primitive's own field
    EXPECT_FLOAT_EQ(prim.color.r, 1.0F);
    EXPECT_EQ(m["overridden"], Json::array({"clearcoat"})); // the rest is an override
    EXPECT_FLOAT_EQ(m["material"]["clearcoat"].get<float>(), 1.0F);
    // scene.get shows the same: primitive fields + the material override.
    const Json comps = ok("scene.get", {{"entity", "ball"}})["components"];
    EXPECT_FLOAT_EQ(comps["primitive"]["roughness"].get<float>(), 0.15F);
    EXPECT_FLOAT_EQ(comps["material"]["clearcoat"].get<float>(), 1.0F);

    m = ok("material.set", {{"entity", "ball"}, {"material", {{"clearcoat", nullptr}}}});
    EXPECT_TRUE(m["overridden"].empty());
    EXPECT_FALSE(reg().all_of<s::MaterialOverride>(entity(created["id"])));
}

TEST_F(WorldTuning, MaterialOfAMeshIsPerEntity) {
    engine::asset::AssetManager assets;
    engine::asset::PbrMaterialParams base;
    base.metallic_factor = 1.0F;
    base.flags = engine::asset::material_has_normal;
    const auto handle = assets.load<engine::asset::MaterialAsset>(
        "test/metal", [&](const std::filesystem::path&) {
            engine::asset::MaterialAsset mat;
            mat.params = base;
            return std::expected<engine::asset::MaterialAsset, engine::core::Error>(std::move(mat));
        });
    const entt::entity a = scene.create_entity("a");
    const entt::entity b = scene.create_entity("b");
    reg().emplace<s::MeshRenderer>(a, engine::asset::AssetHandle<engine::asset::MeshAsset>{}, handle);
    reg().emplace<s::MeshRenderer>(b, engine::asset::AssetHandle<engine::asset::MeshAsset>{}, handle);

    Json m = ok("material.set", {{"entity", "a"}, {"material", {{"roughness", 0.3}, {"metallic", 0}}}});
    EXPECT_EQ(m["source"], "mesh");
    EXPECT_EQ(m["shared_by"], 2);
    EXPECT_EQ(m["textures"], Json::array({"normal"}));
    EXPECT_EQ(m["overridden"], Json::array({"metallic", "roughness"}));
    EXPECT_FLOAT_EQ(m["material"]["metallic"].get<float>(), 0.0F);
    EXPECT_FLOAT_EQ(ok("material.get", {{"entity", "b"}})["material"]["metallic"].get<float>(), 1.0F);

    std::string msg = err("material.set", {{"entity", "a"}, {"material", {{"roughness", 1.5}}}},
                          code::invalid_params);
    EXPECT_TRUE(contains(msg, "material.roughness: expected a number in [0, 1]")) << msg;
    msg = err("material.set", {{"entity", "a"}, {"material", {{"gloss", 1}}}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "material.gloss: unknown field (known: base_color")) << msg;
    // All or nothing: the valid metallic before the bad field is not applied.
    err("material.set", {{"entity", "a"}, {"material", {{"metallic", 1}, {"ior", 9}}}},
        code::invalid_params);
    EXPECT_FLOAT_EQ(ok("material.get", {{"entity", "a"}})["material"]["metallic"].get<float>(), 0.0F);

    err("material.set", {{"entity", "a"}}, code::invalid_params);
    err("material.set", {{"entity", "a"}, {"material", 3}}, code::invalid_params);
    err("material.get", {{"entity", "nobody"}}, code::not_found);
    const entt::entity bare = scene.create_entity("bare");
    (void)bare;
    msg = err("material.get", {{"entity", "bare"}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "has no material")) << msg;
    ok("scene.create", {{"name", "rocks"}, {"components", {{"instanced_mesh", {{"shape", "cube"}}}}}});
    msg = err("material.get", {{"entity", "rocks"}}, code::invalid_params);
    EXPECT_TRUE(contains(msg, "instanced_mesh")) << msg;
}

} // namespace
