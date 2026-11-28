// AppConfig's fixed frame time (#128): bounded runs (QUAT_MAX_FRAMES) advance
// by a fixed 1/60 s so captures are reproducible; QUAT_FIXED_DT overrides.

#include <gtest/gtest.h>

#include "app/app_config.hpp"
#include "support/scoped_env.hpp"

namespace {

using quat::app::app_config_from_environment;
using quat::test::ScopedEnv;

TEST(AppConfigFixedDt, UnboundedRunsUseMeasuredFrameTime) {
    const ScopedEnv frames("QUAT_MAX_FRAMES", nullptr);
    const ScopedEnv fixed("QUAT_FIXED_DT", nullptr);
    EXPECT_FALSE(app_config_from_environment().fixed_dt.has_value());
}

TEST(AppConfigFixedDt, BoundedRunsStepBySixtiethsOfASecond) {
    const ScopedEnv frames("QUAT_MAX_FRAMES", "24");
    const ScopedEnv fixed("QUAT_FIXED_DT", nullptr);
    const auto config = app_config_from_environment();
    ASSERT_TRUE(config.fixed_dt.has_value());
    EXPECT_FLOAT_EQ(*config.fixed_dt, 1.0F / 60.0F);
}

TEST(AppConfigFixedDt, ExplicitValueOverridesAndZeroTurnsItOff) {
    const ScopedEnv frames("QUAT_MAX_FRAMES", "24");
    {
        const ScopedEnv fixed("QUAT_FIXED_DT", "0.02");
        const auto config = app_config_from_environment();
        ASSERT_TRUE(config.fixed_dt.has_value());
        EXPECT_FLOAT_EQ(*config.fixed_dt, 0.02F);
    }
    {
        const ScopedEnv fixed("QUAT_FIXED_DT", "0");
        EXPECT_FALSE(app_config_from_environment().fixed_dt.has_value());
    }
}

TEST(AppConfigFixedDt, MalformedValuesAreIgnored) {
    const ScopedEnv frames("QUAT_MAX_FRAMES", nullptr);
    for (const char* bad : {"abc", "0.1x", "-1", "5", "nan"}) {
        const ScopedEnv fixed("QUAT_FIXED_DT", bad);
        EXPECT_FALSE(app_config_from_environment().fixed_dt.has_value()) << bad;
    }
}

} // namespace
