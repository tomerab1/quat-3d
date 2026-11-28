// RenderSettings JSON codec (A8): the descriptor table drives encode, decode
// and the schema; strict decoding (remote) rejects precisely, lenient
// decoding (scene files) tolerates newer files; edits merge and are atomic.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/renderer/render_settings.hpp"
#include "engine/scene/render_settings_json.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace r = engine::renderer;
namespace s = engine::scene;
using nlohmann::json;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A non-default value for every field.
r::RenderSettings tuned() {
    r::RenderSettings out;
    for (const r::RenderSettingDesc& d : r::render_setting_fields()) {
        float v = 0.0F;
        switch (d.type) {
        case r::RenderSettingType::boolean: v = 1.0F - d.default_value; break;
        case r::RenderSettingType::integer: v = d.default_value == d.max ? d.min : d.max; break;
        case r::RenderSettingType::number: v = d.min + (d.max - d.min) * 0.37F; break;
        case r::RenderSettingType::choice: v = d.default_value == d.max ? d.min : d.max; break;
        case r::RenderSettingType::path:
            (void)r::set_render_setting_text(out, d, "assets/tuned/" + std::string(d.name) +
                                                         std::string(d.unit));
            continue;
        }
        r::set_render_setting(out, d, v);
    }
    return out;
}

TEST(RenderSettingsJson, EncodeDecodeRoundTripsEveryField) {
    const r::RenderSettings source = tuned();
    const json encoded = s::encode_render_settings(source);
    for (const r::RenderSettingGroup& g : r::render_setting_groups()) {
        ASSERT_TRUE(encoded.contains(std::string(g.name))) << g.name;
    }
    r::RenderSettings decoded;
    ASSERT_OK(s::apply_render_settings_json(decoded, encoded, /*strict=*/true));
    for (const r::RenderSettingDesc& d : r::render_setting_fields()) {
        EXPECT_FLOAT_EQ(r::render_setting_value(decoded, d), r::render_setting_value(source, d))
            << d.group << "." << d.name;
        EXPECT_EQ(r::render_setting_text(decoded, d), r::render_setting_text(source, d))
            << d.group << "." << d.name;
        const json& v = encoded[std::string(d.group)][std::string(d.name)];
        switch (d.type) {
        case r::RenderSettingType::boolean: EXPECT_TRUE(v.is_boolean()); break;
        case r::RenderSettingType::integer: EXPECT_TRUE(v.is_number_integer()); break;
        case r::RenderSettingType::number: EXPECT_TRUE(v.is_number_float()); break;
        case r::RenderSettingType::choice:
        case r::RenderSettingType::path: EXPECT_TRUE(v.is_string()); break;
        }
    }
    // One group only.
    const json bloom = s::encode_render_settings(source, "bloom");
    EXPECT_EQ(bloom.size(), 1U);
    EXPECT_TRUE(bloom.contains("bloom"));
}

TEST(RenderSettingsJson, SetMergesAndKeepsOtherFields) {
    r::RenderSettings st;
    st.bloom.threshold = 2.0F;
    ASSERT_OK(s::apply_render_settings_json(
        st, json{{"exposure", {{"compensation_ev", 1.5}}}, {"bloom", {{"intensity", 0.2}}}}, true));
    EXPECT_FLOAT_EQ(st.exposure.compensation_ev, 1.5F);
    EXPECT_FLOAT_EQ(st.bloom.intensity, 0.2F);
    EXPECT_FLOAT_EQ(st.bloom.threshold, 2.0F); // untouched
    EXPECT_TRUE(st.exposure.auto_exposure);
    // Integers accept a whole float; null means "leave as is".
    ASSERT_OK(s::apply_render_settings_json(
        st, json{{"shadows", {{"cascades", 2.0}, {"max_distance", nullptr}}}}, true));
    EXPECT_EQ(st.shadows.cascade_count, 2);
    EXPECT_FLOAT_EQ(st.shadows.max_distance, r::CsmSettings{}.max_distance);
    // Retired pre-cascade keys are accepted (even strictly) and ignored.
    const r::RenderSettings before = st;
    ASSERT_OK(s::apply_render_settings_json(
        st, json{{"shadows", {{"extent_m", 20.0}, {"depth_bias", 0.001}, {"slope_bias", 0.002}}}},
        true));
    EXPECT_EQ(s::encode_render_settings(st), s::encode_render_settings(before));
    EXPECT_FALSE(s::encode_render_settings(st)["shadows"].contains("extent_m"));
}

TEST(RenderSettingsJson, StrictErrorsArePreciseAndAtomic) {
    r::RenderSettings st;
    auto e = s::apply_render_settings_json(st, json{{"fog", {{"density", 1}}}}, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "fog: unknown group")) << e.error().message;
    EXPECT_TRUE(contains(e.error().message, "exposure, bloom")) << e.error().message;

    e = s::apply_render_settings_json(st, json{{"bloom", {{"strength", 1}}}}, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "bloom.strength: unknown field (known: enabled"))
        << e.error().message;

    e = s::apply_render_settings_json(st, json{{"exposure", {{"compensation_ev", 11}}}}, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message,
                         "exposure.compensation_ev: expected a number in [-10, 10] (EV)"))
        << e.error().message;

    e = s::apply_render_settings_json(st, json{{"taa", {{"enabled", 1}}}}, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "taa.enabled: expected true or false"));

    e = s::apply_render_settings_json(st, json{{"shadows", {{"cascades", 1.5}}}}, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "an integer in [1, 4]")) << e.error().message;

    e = s::apply_render_settings_json(st, json{{"bloom", 3}}, true);
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "bloom: expected an object"));

    // A bad field anywhere changes nothing, not even the valid fields before it.
    e = s::apply_render_settings_json(
        st, json{{"bloom", {{"intensity", 0.3}}}, {"taa", {{"feedback", 5}}}}, true);
    ASSERT_FALSE(e);
    EXPECT_FLOAT_EQ(st.bloom.intensity, r::BloomSettings{}.intensity);
}

TEST(RenderSettingsJson, LenientDecodingToleratesNewerFiles) {
    r::RenderSettings st;
    ASSERT_OK(s::apply_render_settings_json(
        st,
        json{{"version", 9},
             {"fog", {{"density", 1}}},
             {"bloom", {{"intensity", 50}, {"future_field", true}}},
             {"exposure", {{"compensation_ev", -1}}}},
        /*strict=*/false));
    EXPECT_FLOAT_EQ(st.bloom.intensity, 1.0F); // clamped into [0, 1]
    EXPECT_FLOAT_EQ(st.exposure.compensation_ev, -1.0F);
    // Wrong types still fail.
    EXPECT_FALSE(s::apply_render_settings_json(st, json{{"bloom", {{"enabled", "yes"}}}}, false));
}

TEST(RenderSettingsJson, GroupCodecUsesTheGroupContext) {
    r::RenderSettings st;
    const r::RenderSettingGroup* clouds = r::find_render_setting_group("clouds");
    ASSERT_NE(clouds, nullptr);
    ASSERT_OK(s::apply_render_setting_group(st, *clouds, json{{"coverage", 0.9}}, true, "clouds"));
    EXPECT_FLOAT_EQ(st.clouds.coverage, 0.9F);
    auto e = s::apply_render_setting_group(st, *clouds, json{{"coverage", 2}}, true, "clouds");
    ASSERT_FALSE(e);
    EXPECT_TRUE(contains(e.error().message, "clouds.coverage: expected a number in [0, 1]"));
    const json enc = s::encode_render_setting_group(st, *clouds);
    EXPECT_FLOAT_EQ(enc["coverage"].get<float>(), 0.9F);
    EXPECT_FALSE(enc.contains("time_s")); // frame-loop state is not a setting
}

TEST(RenderSettingsJson, SchemaMirrorsTheTable) {
    const json schema = s::render_settings_schema();
    ASSERT_TRUE(schema["groups"].is_array());
    std::size_t fields = 0;
    for (const json& g : schema["groups"]) {
        ASSERT_TRUE(g.contains("name") && g.contains("label") && g.contains("hint"));
        for (const json& f : g["fields"]) {
            ++fields;
            const auto* d = r::find_render_setting(g["name"].get<std::string>(),
                                                   f["name"].get<std::string>());
            ASSERT_NE(d, nullptr) << f.dump();
            const std::string type = f["type"].get<std::string>();
            if (type == "boolean") {
                EXPECT_FALSE(f.contains("min"));
                EXPECT_TRUE(f["default"].is_boolean());
            } else if (d->type == r::RenderSettingType::choice) {
                EXPECT_EQ(type, "string");
                ASSERT_TRUE(f["enum"].is_array());
                EXPECT_EQ(f["enum"].size(), d->choices.size());
                EXPECT_TRUE(f["default"].is_string());
            } else if (d->type == r::RenderSettingType::path) {
                EXPECT_EQ(type, "string");
                EXPECT_EQ(f["format"], "path");
                EXPECT_EQ(f["extension"].get<std::string>(), d->unit);
            } else {
                EXPECT_FLOAT_EQ(f["min"].get<float>(), d->min);
                EXPECT_FLOAT_EQ(f["max"].get<float>(), d->max);
            }
            EXPECT_EQ(f["hint"].get<std::string>(), d->hint);
        }
    }
    EXPECT_EQ(fields, r::render_setting_fields().size());
    EXPECT_EQ(schema["groups"][0]["fields"][1]["unit"], "EV"); // exposure.compensation_ev
}

// R11: the upscaler's enums travel by name; anything else is rejected
// strictly, with the list of names, and leaves the settings untouched.
TEST(RenderSettingsJson, ChoiceFieldsUseNames) {
    r::RenderSettings settings;
    ASSERT_OK(s::apply_render_settings_json(
        settings, json{{"upscaler", {{"mode", "performance"}, {"backend", "fsr2"}}}}, true));
    EXPECT_EQ(r::upscaler_mode(settings.upscaler), r::UpscalerMode::performance);
    EXPECT_EQ(r::upscaler_backend(settings.upscaler), r::UpscalerBackend::fsr2);
    EXPECT_EQ(s::encode_render_settings(settings, "upscaler")["upscaler"]["mode"], "performance");

    for (const json& bad : {json("ultra"), json(2), json(true)}) {
        r::RenderSettings untouched = settings;
        auto result =
            s::apply_render_settings_json(untouched, json{{"upscaler", {{"mode", bad}}}}, true);
        ASSERT_FALSE(result.has_value()) << bad.dump();
        EXPECT_TRUE(contains(result.error().message, "upscaler.mode")) << result.error().message;
        EXPECT_TRUE(contains(result.error().message, "\"balanced\"")) << result.error().message;
        EXPECT_EQ(untouched.upscaler.mode, settings.upscaler.mode);
    }
    // The default mode is stored by name as "auto" (resolved per display at
    // render time), so scenes stay portable between HiDPI and standard machines.
    EXPECT_EQ(s::encode_render_settings(r::RenderSettings{}, "upscaler")["upscaler"]["mode"],
              "auto");
    ASSERT_OK(s::apply_render_settings_json(settings, json{{"upscaler", {{"mode", "auto"}}}},
                                            true));
    EXPECT_EQ(r::upscaler_mode(settings.upscaler), r::UpscalerMode::automatic);
}

} // namespace

namespace {

// R20: a `path` field travels as a string; strict decoding (remote) names
// what is wrong with a bad path and changes nothing, lenient decoding (scene
// files) clears it and keeps loading.
TEST(RenderSettingsJson, PathFieldsAreCheckedStrings) {
    r::RenderSettings st;
    ASSERT_OK(s::apply_render_settings_json(
        st, json{{"grading", {{"lut", "assets/luts/warm.cube"}, {"contrast", 1.2}}}}, true));
    EXPECT_EQ(st.grading.lut, "assets/luts/warm.cube");
    EXPECT_EQ(s::encode_render_settings(st, "grading")["grading"]["lut"], "assets/luts/warm.cube");

    const r::RenderSettings before = st;
    auto bad = s::apply_render_settings_json(
        st, json{{"grading", {{"contrast", 0.5}, {"lut", "../../etc/x.cube"}}}}, true);
    ASSERT_FALSE(bad.has_value());
    EXPECT_TRUE(contains(bad.error().message, "grading.lut")) << bad.error().message;
    EXPECT_TRUE(contains(bad.error().message, "..")) << bad.error().message;
    EXPECT_FLOAT_EQ(st.grading.contrast, before.grading.contrast); // atomic
    EXPECT_FALSE(s::apply_render_settings_json(st, json{{"grading", {{"lut", 3}}}}, true));

    ASSERT_OK(s::apply_render_settings_json(st, json{{"grading", {{"lut", "/abs.cube"}}}}, false));
    EXPECT_EQ(st.grading.lut, "");
}

} // namespace
