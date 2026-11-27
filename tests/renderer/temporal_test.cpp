// CPU side of the R11 temporal contract: the Halton jitter source, upscaler
// settings, the velocity reference, MotionHistory and the per-instance motion
// records the draw lists carry (no GPU needed).

#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/jitter.hpp"
#include "engine/renderer/motion_history.hpp"
#include "engine/renderer/upscaler_settings.hpp"
#include "engine/renderer/velocity.hpp"

namespace {

namespace r = engine::renderer;

bool same(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c) {
        if (a[c] != b[c]) return false;
    }
    return true;
}

glm::mat4 translation(float x, float y, float z) {
    return glm::translate(glm::mat4(1.0F), glm::vec3(x, y, z));
}

} // namespace

TEST(Jitter, HaltonOffsetsStayInsideThePixelAndCycle) {
    constexpr VkExtent2D render{320, 180};
    const std::uint32_t phases = r::jitter_phase_count(render, render);
    EXPECT_EQ(phases, 8U);
    std::set<std::pair<float, float>> distinct;
    for (std::uint64_t i = 0; i < phases; ++i) {
        const r::ViewJitter j = r::halton_jitter(i, render, phases);
        EXPECT_GE(j.pixels.x, -0.5F);
        EXPECT_LT(j.pixels.x, 0.5F);
        EXPECT_GE(j.pixels.y, -0.5F);
        EXPECT_LT(j.pixels.y, 0.5F);
        EXPECT_FLOAT_EQ(j.ndc.x, j.pixels.x * 2.0F / 320.0F);
        EXPECT_FLOAT_EQ(j.ndc.y, j.pixels.y * 2.0F / 180.0F);
        distinct.insert({j.pixels.x, j.pixels.y});
        const r::ViewJitter again = r::halton_jitter(i + phases, render, phases);
        EXPECT_EQ(again.pixels, j.pixels) << "the sequence must repeat every phase_count frames";
    }
    EXPECT_EQ(distinct.size(), phases);
}

TEST(Jitter, PhaseCountGrowsWithTheUpscaleArea) {
    EXPECT_EQ(r::jitter_phase_count({960, 540}, {1920, 1080}), 32U);
    EXPECT_EQ(r::jitter_phase_count({1280, 720}, {1920, 1080}), 18U);
    EXPECT_EQ(r::jitter_phase_count({0, 0}, {1920, 1080}), 8U);
}

// The jittered matrix shifts every projected point by exactly the NDC jitter.
TEST(Jitter, JitteredViewProjOffsetsNdc) {
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.5F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 vp = proj * glm::lookAt(glm::vec3(1.0F, 2.0F, 5.0F), glm::vec3(0.0F),
                                            glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec2 jitter(0.003F, -0.002F);
    const glm::mat4 jvp = r::jitter_view_proj(vp, jitter);
    const glm::vec4 p(0.3F, -0.4F, 0.5F, 1.0F);
    const glm::vec4 a = vp * p;
    const glm::vec4 b = jvp * p;
    EXPECT_NEAR(b.x / b.w - a.x / a.w, jitter.x, 1e-6F);
    EXPECT_NEAR(b.y / b.w - a.y / a.w, jitter.y, 1e-6F);
    EXPECT_FLOAT_EQ(b.z, a.z) << "jitter must not change depth";
}

TEST(Jitter, TextureLodBiasFollowsTheRenderScale) {
    EXPECT_FLOAT_EQ(r::texture_lod_bias({960, 540}, {1920, 1080}), -1.0F);
    EXPECT_FLOAT_EQ(r::texture_lod_bias({1920, 1080}, {1920, 1080}), 0.0F);
    EXPECT_NEAR(r::texture_lod_bias({1280, 720}, {1920, 1080}), std::log2(2.0F / 3.0F), 1e-6F);
}

TEST(UpscalerSettings, ModesMapToRenderExtents) {
    r::UpscalerSettings s;
    EXPECT_EQ(r::upscaler_render_extent({1920, 1080}, s).width, 1920U);
    s.mode = r::UpscalerMode::quality;
    EXPECT_EQ(r::upscaler_render_extent({1920, 1080}, s).width, 1280U);
    EXPECT_EQ(r::upscaler_render_extent({1920, 1080}, s).height, 720U);
    s.mode = r::UpscalerMode::balanced;
    EXPECT_EQ(r::upscaler_render_extent({1920, 1080}, s).width, 1129U);
    s.mode = r::UpscalerMode::performance;
    EXPECT_EQ(r::upscaler_render_extent({1920, 1080}, s).height, 540U);
    // Override, clamped to [0.25, 1]; never zero for a non-empty display.
    s.render_scale = 0.75F;
    EXPECT_EQ(r::upscaler_render_extent({1920, 1080}, s).width, 1440U);
    s.render_scale = 0.01F;
    EXPECT_FLOAT_EQ(r::upscaler_render_scale(s), 0.25F);
    s.render_scale = 3.0F;
    EXPECT_FLOAT_EQ(r::upscaler_render_scale(s), 1.0F);
    s.render_scale = 0.25F;
    EXPECT_EQ(r::upscaler_render_extent({2, 1}, s).height, 1U);
}

TEST(UpscalerSettings, NamesRoundTrip) {
    for (const r::UpscalerMode m : {r::UpscalerMode::native_aa, r::UpscalerMode::quality,
                                    r::UpscalerMode::balanced, r::UpscalerMode::performance}) {
        EXPECT_EQ(r::parse_upscaler_mode(r::to_string(m)), m);
    }
    for (const r::UpscalerBackend b : {r::UpscalerBackend::taau, r::UpscalerBackend::fsr2}) {
        EXPECT_EQ(r::parse_upscaler_backend(r::to_string(b)), b);
    }
    EXPECT_FALSE(r::parse_upscaler_mode("ultra").has_value());
    EXPECT_FALSE(r::parse_upscaler_backend("dlss").has_value());
}

TEST(Velocity, ReferenceIsUvMotionOfThePoint) {
    const glm::mat4 vp = glm::ortho(-1.0F, 1.0F, -1.0F, 1.0F, -1.0F, 1.0F);
    // 0.1 world units right = 0.05 of the target width.
    const glm::vec2 v = r::surface_velocity(glm::vec3(0.1F, 0.0F, 0.0F), glm::vec3(0.0F), vp, vp);
    EXPECT_NEAR(v.x, 0.05F, 1e-6F);
    EXPECT_NEAR(v.y, 0.0F, 1e-6F);
    // A static point under a moving camera moves opposite to the camera.
    const glm::mat4 prev_vp = vp * translation(-0.2F, 0.0F, 0.0F);
    const glm::vec2 cam = r::surface_velocity(glm::vec3(0.0F), glm::vec3(0.0F), vp, prev_vp);
    EXPECT_NEAR(cam.x, 0.1F, 1e-6F);
}

TEST(MotionHistory, ReturnsLastFramesTransform) {
    r::MotionHistory h;
    h.begin_frame();
    const glm::mat4 a = translation(1.0F, 0.0F, 0.0F);
    EXPECT_TRUE(same(h.advance(7, a), a)) << "a new key has no previous transform";
    h.begin_frame();
    const glm::mat4 b = translation(2.0F, 0.0F, 0.0F);
    EXPECT_TRUE(same(h.advance(7, b), a));
    EXPECT_TRUE(same(h.advance(7, b), a)) << "a second call in the frame sees the same value";
    h.begin_frame();
    // Key 7 is skipped this frame: it drops out and comes back as new.
    EXPECT_EQ(h.tracked_transforms(), 0U);
    h.begin_frame();
    const glm::mat4 c = translation(3.0F, 0.0F, 0.0F);
    EXPECT_TRUE(same(h.advance(7, c), c));
    h.reset();
    h.begin_frame();
    EXPECT_TRUE(same(h.advance(7, a), a)) << "reset drops all history";
}

TEST(MotionHistory, PalettesNeedMatchingSizes) {
    r::MotionHistory h;
    const std::array<glm::mat4, 2> p0{translation(1, 0, 0), translation(0, 1, 0)};
    const std::array<glm::mat4, 2> p1{translation(2, 0, 0), translation(0, 2, 0)};
    const std::array<glm::mat4, 3> p2{};
    h.begin_frame();
    auto first = h.advance_palette(1, p0);
    ASSERT_EQ(first.size(), 2U);
    EXPECT_TRUE(same(first[0], p0[0]));
    h.begin_frame();
    auto second = h.advance_palette(1, p1);
    ASSERT_EQ(second.size(), 2U);
    EXPECT_TRUE(same(second[1], p0[1]));
    h.begin_frame();
    auto resized = h.advance_palette(1, p2);
    EXPECT_EQ(resized.size(), 3U) << "a joint-count change falls back to the current palette";
}

TEST(DrawBatching, MotionRecordsFollowTheirInstances) {
    engine::asset::MeshAsset mesh_a;
    engine::asset::MeshAsset mesh_b;
    std::array<r::DrawItem, 3> items{};
    items[0].mesh = &mesh_a;
    items[0].model = translation(1, 0, 0);
    items[0].prev_model = translation(0, 0, 0);
    items[1].mesh = &mesh_b; // no previous pose: camera-only
    items[1].model = translation(5, 0, 0);
    items[2].mesh = &mesh_a;
    items[2].model = translation(2, 0, 0);
    items[2].prev_model = translation(3, 0, 0);
    items[2].prev_positions = 0x1000;
    const r::InstancedDrawList list = r::batch_draws(items);
    ASSERT_EQ(list.motion.size(), list.instances.size());
    for (std::size_t i = 0; i < list.instances.size(); ++i) {
        const glm::mat4& model = list.instances[i].model;
        const r::InstanceMotion& m = list.motion[i];
        if (same(model, items[0].model)) {
            EXPECT_TRUE(same(m.prev_model, *items[0].prev_model));
            EXPECT_EQ(m.has_prev_positions, 0U);
        } else if (same(model, items[1].model)) {
            EXPECT_TRUE(same(m.prev_model, items[1].model));
        } else {
            EXPECT_TRUE(same(m.prev_model, *items[2].prev_model));
            EXPECT_EQ(m.prev_positions, 0x1000U);
            EXPECT_EQ(m.has_prev_positions, 1U);
        }
    }
    const r::InstancedDrawList flat = r::unbatched_draws(items);
    ASSERT_EQ(flat.motion.size(), 3U);
    EXPECT_TRUE(same(flat.motion[2].prev_model, *items[2].prev_model));
}
