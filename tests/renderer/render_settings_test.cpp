// RenderSettings descriptor table (A8): every entry points at the field it
// names, its default is the struct's default, and the setters clamp.

#include <set>
#include <string>

#include <gtest/gtest.h>

#include "engine/renderer/render_settings.hpp"

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
    r::set_render_setting(s, *pcf, 2.6F);
    EXPECT_EQ(s.shadows.pcf_radius, 3);
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

} // namespace
