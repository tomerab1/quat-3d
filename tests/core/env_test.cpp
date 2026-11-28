// engine::core::env_utf8 / env_path: environment values with non-ASCII
// characters arrive intact (Windows: read from the UTF-16 environment, not
// the ANSI code page std::getenv would use).

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"

namespace {

namespace fs = std::filesystem;
using engine::core::env_path;
using engine::core::env_utf8;

constexpr const char* k_name = "QUAT_ENV_TEST_PATH";

// "スクリーン dir/ünï.ppm": CJK (outside every Western ANSI code page) plus
// Latin-1, spelled with escapes so the source charset does not matter.
const char8_t* const k_value_u8 = u8"スクリーン dir/ünï.ppm";

void set_env(const fs::path& value) {
#if defined(_WIN32)
    const std::wstring name(k_name, k_name + std::char_traits<char>::length(k_name));
    ASSERT_EQ(::_wputenv_s(name.c_str(), value.c_str()), 0);
#else
    ASSERT_EQ(::setenv(k_name, value.c_str(), 1), 0);
#endif
}

void unset_env() {
#if defined(_WIN32)
    const std::wstring name(k_name, k_name + std::char_traits<char>::length(k_name));
    (void)::_wputenv_s(name.c_str(), L"");
#else
    (void)::unsetenv(k_name);
#endif
}

TEST(Env, ReadsNonAsciiPathsIntact) {
    const fs::path expected{std::u8string_view(k_value_u8)};
    set_env(expected);
    const auto path = env_path(k_name);
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(*path, expected);
    const auto utf8 = env_utf8(k_name);
    ASSERT_TRUE(utf8.has_value());
    EXPECT_EQ(*utf8, engine::core::path_to_utf8(expected));
    EXPECT_EQ(engine::core::path_from_utf8(*utf8), expected);
    unset_env();
}

TEST(Env, UnsetOrEmptyIsNoPath) {
    unset_env();
    EXPECT_FALSE(env_path(k_name).has_value());
    EXPECT_FALSE(env_path("QUAT_ENV_TEST_DEFINITELY_UNSET").has_value());
    EXPECT_FALSE(env_utf8("QUAT_ENV_TEST_DEFINITELY_UNSET").has_value());
}

} // namespace

// Boolean switches (#119: QUAT_GAME_CAMERA=0 used to turn the game camera on).
TEST(EnvFlag, ParsesTheUsualSpellingsAndRejectsTheRest) {
    using engine::core::parse_env_flag;
    for (const char* on : {"1", "true", "TRUE", "Yes", "on", " 1 "}) {
        EXPECT_EQ(parse_env_flag(on), std::optional<bool>(true)) << on;
    }
    for (const char* off : {"0", "false", "False", "no", "OFF", "", "  "}) {
        EXPECT_EQ(parse_env_flag(off), std::optional<bool>(false)) << off;
    }
    for (const char* bad : {"2", "enable", "0x1", "truee"}) {
        EXPECT_EQ(parse_env_flag(bad), std::nullopt) << bad;
    }
}

TEST(EnvFlag, UnsetUsesTheFallbackAndZeroIsOff) {
    using engine::core::env_flag;
    constexpr const char* k_flag = "QUAT_ENV_TEST_FLAG";
#if defined(_WIN32)
    const auto set = [&](const char* v) { ASSERT_EQ(::_putenv_s(k_flag, v), 0); };
    const auto unset = [&] { (void)::_putenv_s(k_flag, ""); };
#else
    const auto set = [&](const char* v) { ASSERT_EQ(::setenv(k_flag, v, 1), 0); };
    const auto unset = [&] { (void)::unsetenv(k_flag); };
#endif
    unset();
    EXPECT_FALSE(env_flag(k_flag));
    EXPECT_TRUE(env_flag(k_flag, true));
    set("0");
    EXPECT_FALSE(env_flag(k_flag, true));
    set("1");
    EXPECT_TRUE(env_flag(k_flag));
    set("off");
    EXPECT_FALSE(env_flag(k_flag, true));
    set("maybe"); // set, but not a boolean: on (with a warning)
    EXPECT_TRUE(env_flag(k_flag));
    unset();
}
