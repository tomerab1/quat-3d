// engine::core::env_utf8 / env_path: environment values with non-ASCII
// characters arrive intact (Windows: read from the UTF-16 environment, not
// the ANSI code page std::getenv would use).

#include <cstdlib>
#include <filesystem>
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
