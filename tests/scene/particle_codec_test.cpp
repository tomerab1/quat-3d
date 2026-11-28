// ParticleEmitter component codec (R10.3): scene files + remote API.

#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;
namespace r = engine::renderer;

class ParticleCodec : public ::testing::Test {
protected:
    entt::registry reg;
    entt::entity   e = reg.create();

    [[nodiscard]] auto apply(const json& v, bool strict = true) {
        return s::apply_component(reg, e, "particle_emitter", v, strict);
    }
    [[nodiscard]] const r::ParticleEmitterSettings& emitter() const {
        return reg.get<s::ParticleEmitter>(e).settings;
    }
};

TEST_F(ParticleCodec, RoundTripsEveryField) {
    const json authored = {
        {"enabled", false},
        {"seed", 7},
        {"max_particles", 3000},
        {"rate", 250.5},
        {"burst_count", 40},
        {"burst_interval", 1.5},
        {"lifetime", {0.5, 2.5}},
        {"shape", "cone"},
        {"radius", 0.25},
        {"box", {1, 2, 3}},
        {"cone_angle", 35},
        {"direction", {0, 0, 1}},
        {"spread", 12},
        {"speed", {3, 6}},
        {"gravity", {0, -4, 0}},
        {"drag", 0.75},
        {"size", {0.1, 0.3}},
        {"rotation", {-90, 90}},
        {"spin", {-30, 30}},
        {"size_curve", json::array({{{"t", 0}, {"size", 0.5}}, {{"t", 1}, {"size", 2}}})},
        {"color_curve",
         json::array({{{"t", 0}, {"color", {1, 0.5, 0.1}}, {"alpha", 1}},
                      {{"t", 0.6}, {"color", {0.4, 0.4, 0.4}}, {"alpha", 0.5}},
                      {{"t", 1}, {"color", {0.2, 0.2, 0.2}}, {"alpha", 0}}})},
        {"emissive", 4},
        {"texture", "assets/textures/smoke_8x8.png"},
        {"flipbook", {8, 8}},
        {"frames", 60},
        {"fps", 24},
        {"blend", "alpha"},
        {"lighting", "lit"},
        {"space", "local"},
        {"soft_distance", 0.5},
        {"sort", true},
        {"collide", true},
        {"bounce", 0.6},
        {"friction", 0.1}};
    ASSERT_OK(apply(authored));
    const r::ParticleEmitterSettings& p = emitter();
    EXPECT_FALSE(p.enabled);
    EXPECT_EQ(p.max_particles, 3000U);
    EXPECT_EQ(p.shape, r::ParticleShape::cone);
    EXPECT_EQ(p.lifetime, glm::vec2(0.5F, 2.5F));
    EXPECT_EQ(p.color_keys, 3U);
    EXPECT_FLOAT_EQ(p.color_curve[1].color.a, 0.5F);
    EXPECT_EQ(p.size_keys, 2U);
    EXPECT_EQ(p.flipbook_grid, glm::uvec2(8, 8));
    EXPECT_EQ(p.blend, r::ParticleBlend::alpha);
    EXPECT_EQ(p.lighting, r::ParticleLighting::lit);
    EXPECT_EQ(p.space, r::ParticleSpace::local);
    EXPECT_TRUE(p.collide);

    // encode -> decode on a fresh entity gives the same component.
    const json encoded = s::encode_components(reg, e).at("particle_emitter");
    EXPECT_EQ(encoded.size(), authored.size());
    const entt::entity copy = reg.create();
    ASSERT_OK(s::apply_component(reg, copy, "particle_emitter", encoded, true));
    EXPECT_EQ(s::encode_components(reg, copy).at("particle_emitter"), encoded);
    EXPECT_EQ(s::component_names(reg, copy).back(), "particle_emitter");
}

TEST_F(ParticleCodec, MergesIntoTheCurrentValues) {
    ASSERT_OK(apply({{"rate", 10}}));
    ASSERT_OK(apply({{"blend", "alpha"}}));
    EXPECT_FLOAT_EQ(emitter().rate, 10.0F);
    EXPECT_EQ(emitter().blend, r::ParticleBlend::alpha);
    // A hex colour is sRGB (converted to linear), its alpha byte the key's alpha.
    ASSERT_OK(apply({{"color_curve", json::array({{{"t", 0}, {"color", "#ffffff80"}}})}}));
    EXPECT_EQ(emitter().color_keys, 1U);
    EXPECT_NEAR(emitter().color_curve[0].color.a, 128.0F / 255.0F, 1e-6F);
}

TEST_F(ParticleCodec, RejectsInvalidValuesWithTheirPath) {
    const auto error = [&](const json& v) {
        auto ok = apply(v);
        return ok ? std::string() : ok.error().message;
    };
    EXPECT_NE(error({{"texture", "../secret.png"}}).find("texture"), std::string::npos);
    EXPECT_NE(error({{"texture", "cooked/a.png"}}).find("assets/"), std::string::npos);
    EXPECT_NE(error({{"texture", "/etc/passwd"}}).find("texture"), std::string::npos);
    EXPECT_NE(error({{"max_particles", 0}}).find("max_particles"), std::string::npos);
    EXPECT_NE(error({{"max_particles", 100000}}).find("max_particles"), std::string::npos);
    EXPECT_NE(error({{"lifetime", {2, 1}}}).find("lifetime"), std::string::npos);
    EXPECT_NE(error({{"shape", "torus"}}).find("shape"), std::string::npos);
    EXPECT_NE(error({{"direction", {0, 0, 0}}}).find("direction"), std::string::npos);
    EXPECT_NE(error({{"flipbook", {4, 4}}, {"frames", 17}}).find("frames"), std::string::npos);
    EXPECT_NE(error({{"size_curve", json::array({{{"t", 0.8}, {"size", 1}}, {{"t", 0.2}, {"size", 1}}})}})
                  .find("ascending"),
              std::string::npos);
    EXPECT_NE(error({{"color_curve", json::array({{{"t", 0}, {"glow", 1}}})}}).find("glow"),
              std::string::npos);
    EXPECT_NE(error({{"bounce", 2}}).find("bounce"), std::string::npos);
    EXPECT_NE(error({{"speeed", 2}}).find("unknown field"), std::string::npos);
    EXPECT_FALSE(reg.all_of<s::ParticleEmitter>(e)); // nothing half-applied
    // Lenient (scene files): unknown fields are ignored.
    ASSERT_OK(apply({{"speeed", 2}}, false));
}

TEST_F(ParticleCodec, IsListedAndRemovable) {
    const s::ComponentInfo* info = s::find_component_info("particle_emitter");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->addable);
    ASSERT_OK(apply(json::object()));
    EXPECT_TRUE(s::remove_component(reg, e, "particle_emitter"));
    EXPECT_FALSE(reg.all_of<s::ParticleEmitter>(e));
}

} // namespace
