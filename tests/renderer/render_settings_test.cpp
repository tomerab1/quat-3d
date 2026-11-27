// RenderSettings descriptor table (A8): every entry points at the field it
// names, its default is the struct's default, and the setters clamp; the
// shadow distance sizes the cascades; retired keys are listed as deprecated.
//
// What these CPU tests do not cover: the frame graph's per-frame decisions
// (src/app/frame_graph.cpp) — bloom.enabled = false skipping the bloom pass,
// shadows.enabled = false skipping the shadow pass, the TAA jitter switch —
// live in the app layer, which engine_tests does not link. Each pass's side is
// covered by Gpu* tests (render_settings_gpu_test.cpp: exposure, TAA off;
// lighting_pass_test.cpp: no shadow map, PCF radius, bias, IBL intensity;
// csm_gpu_test.cpp / gtao_test.cpp / contact_shadow_test.cpp: R12 groups) and
// the wiring by CI's demo render.

#include <cmath>
#include <set>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/render_settings.hpp"
#include "engine/renderer/csm.hpp"
#include "engine/renderer/shadow_pass.hpp"

namespace {

namespace r = engine::renderer;

TEST(RenderSettings, TableEntriesAreConsistent) {
    std::set<std::string> keys;
    const r::RenderSettings defaults{};
    for (const r::RenderSettingDesc& d : r::render_setting_fields()) {
        const std::string key = std::string(d.group) + "." + std::string(d.name);
        EXPECT_TRUE(keys.insert(key).second) << "duplicate " << key;
        EXPECT_NE(r::find_render_setting_group(d.group), nullptr) << key;
        EXPECT_FALSE(d.label.empty()) << key;
        EXPECT_FALSE(d.hint.empty()) << key;
        EXPECT_LE(d.min, d.default_value) << key;
        EXPECT_GE(d.max, d.default_value) << key;
        EXPECT_LT(d.min, d.max) << key;
        // The table default is the struct default.
        EXPECT_FLOAT_EQ(r::render_setting_value(defaults, d), d.default_value) << key;
        // The reference matches the declared type.
        r::RenderSettings s;
        const r::RenderSettingRef ref = d.ref(s);
        const int set = (ref.b != nullptr) + (ref.f != nullptr) + (ref.i != nullptr);
        EXPECT_EQ(set, 1) << key;
        switch (d.type) {
        case r::RenderSettingType::boolean: EXPECT_NE(ref.b, nullptr) << key; break;
        case r::RenderSettingType::number: EXPECT_NE(ref.f, nullptr) << key; break;
        case r::RenderSettingType::integer: EXPECT_NE(ref.i, nullptr) << key; break;
        case r::RenderSettingType::choice:
            // An index into the names: the range covers exactly the choices.
            EXPECT_NE(ref.i, nullptr) << key;
            EXPECT_EQ(d.min, 0.0F) << key;
            EXPECT_EQ(static_cast<std::size_t>(d.max) + 1, d.choices.size()) << key;
            break;
        }
        EXPECT_EQ(r::find_render_setting(d.group, d.name), &d) << key;
    }
    // Every group lists at least one field.
    for (const r::RenderSettingGroup& g : r::render_setting_groups()) {
        bool any = false;
        for (const r::RenderSettingDesc& d : r::render_setting_fields()) any = any || d.group == g.name;
        EXPECT_TRUE(any) << g.name;
        EXPECT_FALSE(g.label.empty());
    }
}

TEST(RenderSettings, EntriesReachDistinctStorage) {
    // Writing each field to a non-default value changes exactly that field.
    for (const r::RenderSettingDesc& d : r::render_setting_fields()) {
        r::RenderSettings s;
        const float other = d.type == r::RenderSettingType::boolean
                                ? 1.0F - d.default_value
                                : (d.default_value < d.max ? d.max : d.min);
        r::set_render_setting(s, d, other);
        for (const r::RenderSettingDesc& e : r::render_setting_fields()) {
            const float expected = &e == &d ? other : e.default_value;
            EXPECT_FLOAT_EQ(r::render_setting_value(s, e), expected)
                << d.group << "." << d.name << " leaked into " << e.group << "." << e.name;
        }
    }
}

TEST(RenderSettings, SetClampsAndRounds) {
    r::RenderSettings s;
    const r::RenderSettingDesc* ev = r::find_render_setting("exposure", "compensation_ev");
    ASSERT_NE(ev, nullptr);
    r::set_render_setting(s, *ev, 100.0F);
    EXPECT_FLOAT_EQ(s.exposure.compensation_ev, ev->max);
    const r::RenderSettingDesc* pcf = r::find_render_setting("shadows", "pcf_radius");
    ASSERT_NE(pcf, nullptr);
    r::set_render_setting(s, *pcf, 100.0F);
    EXPECT_FLOAT_EQ(s.shadows.pcf_radius, r::CsmSettings::k_pcf_radius.max);
    const r::RenderSettingDesc* cascades = r::find_render_setting("shadows", "cascades");
    ASSERT_NE(cascades, nullptr);
    r::set_render_setting(s, *cascades, 2.6F);
    EXPECT_EQ(s.shadows.cascade_count, 3);
    const r::RenderSettingDesc* slices = r::find_render_setting("ao", "slices");
    ASSERT_NE(slices, nullptr);
    r::set_render_setting(s, *slices, 0.0F);
    EXPECT_EQ(s.ao.slice_count, r::GtaoSettings::k_slice_count.min);
    const r::RenderSettingDesc* taa = r::find_render_setting("taa", "enabled");
    ASSERT_NE(taa, nullptr);
    r::set_render_setting(s, *taa, 0.0F);
    EXPECT_FALSE(s.taa.enabled);
    EXPECT_EQ(r::find_render_setting("exposure", "nope"), nullptr);
}

TEST(RenderSettings, ResetRestoresOneGroupOrAll) {
    r::RenderSettings s;
    s.bloom.intensity = 0.5F;
    s.exposure.compensation_ev = 2.0F;
    s.clouds.time_s = 42.0F; // frame-loop state survives a reset
    ASSERT_TRUE(r::reset_render_settings(s, "bloom"));
    EXPECT_FLOAT_EQ(s.bloom.intensity, r::BloomSettings{}.intensity);
    EXPECT_FLOAT_EQ(s.exposure.compensation_ev, 2.0F);
    EXPECT_FALSE(r::reset_render_settings(s, "fog"));
    ASSERT_TRUE(r::reset_render_settings(s));
    EXPECT_FLOAT_EQ(s.exposure.compensation_ev, 0.0F);
    EXPECT_FLOAT_EQ(s.clouds.time_s, 42.0F);
}

// shadows.max_distance (which replaced the fixed box's extent_m) is where the
// cascades end: a receiver nearer than it lands inside the last cascade's
// clip square, one beyond it is past the last split and unshadowed.
TEST(RenderSettings, ShadowDistanceSizesTheCascades) {
    r::CsmView view;
    view.view = glm::lookAt(glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.0F, 2.0F, -1.0F),
                            glm::vec3(0.0F, 1.0F, 0.0F));
    view.projection = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 1000.0F);
    view.projection[1][1] *= -1.0F;
    view.to_light = glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F));
    const auto covered = [&](float distance, float at) {
        r::RenderSettings s;
        s.shadows.max_distance = distance;
        s.shadows.gpu_depth_bounds = false; // the CPU fit over near .. max_distance
        const glm::vec2 range = r::shadow_range(view.projection, s.shadows, false, 0, 0).range;
        const r::GpuCascades cs = r::fit_cascades(view, s.shadows, range, r::shadow_map_size);
        if (at >= cs.split_far[3]) return false;
        const glm::vec4 c = cs.view_proj[3] * glm::vec4(0.0F, 0.0F, -at, 1.0F);
        return std::abs(c.x) <= 1.0F && std::abs(c.y) <= 1.0F && c.z >= 0.0F && c.z <= 1.0F;
    };
    const float distance = r::CsmSettings{}.max_distance;
    EXPECT_TRUE(covered(distance, 0.7F * distance));
    EXPECT_FALSE(covered(distance, 1.2F * distance));
    EXPECT_TRUE(covered(3.0F * distance, 1.2F * distance));
}

// The pre-cascade shadow keys are retired, not forgotten: listed as
// deprecated, not as fields.
TEST(RenderSettings, RetiredShadowKeysAreDeprecated) {
    for (const char* key : {"extent_m", "depth_bias", "slope_bias"}) {
        EXPECT_EQ(r::find_render_setting("shadows", key), nullptr) << key;
        const r::DeprecatedRenderSetting* d = r::find_deprecated_render_setting("shadows", key);
        ASSERT_NE(d, nullptr) << key;
        EXPECT_FALSE(d->note.empty());
    }
    EXPECT_EQ(r::find_deprecated_render_setting("shadows", "pcf_radius"), nullptr);
}

} // namespace
