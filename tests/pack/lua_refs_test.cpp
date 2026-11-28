// The Lua literal scanner the packager follows scripts with.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/pack/lua_refs.hpp"

namespace {

using engine::pack::lua_string_literals;

std::vector<std::string> texts(std::string_view src) {
    std::vector<std::string> out;
    for (const auto& l : lua_string_literals(src)) out.push_back(l.text);
    return out;
}

TEST(LuaRefs, FindsStringLiterals) {
    const auto lits = lua_string_literals(
        "local a = \"coin_collector/coin.lua\"\n"
        "local b = 'assets/meshes/crate.glb'\n"
        "print(\"esc\\\"aped\\n\")\n");
    ASSERT_EQ(lits.size(), 3U);
    EXPECT_EQ(lits[0].text, "coin_collector/coin.lua");
    EXPECT_EQ(lits[0].line, 1U);
    EXPECT_EQ(lits[1].text, "assets/meshes/crate.glb");
    EXPECT_EQ(lits[1].line, 2U);
    EXPECT_EQ(lits[2].text, "esc\"aped\n");
    EXPECT_FALSE(lits[0].required);
}

TEST(LuaRefs, SkipsComments) {
    EXPECT_EQ(texts("-- \"not.lua\"\nlocal x = 1 --[[ 'nor.lua'\n ]] local y = 'yes.lua'\n"
                    "--[==[ ]] still a comment \"no.lua\" ]==]"),
              std::vector<std::string>{"yes.lua"});
}

TEST(LuaRefs, LongStrings) {
    const auto lits = lua_string_literals("local s = [[\nfirst]] local t = [==[a]]b]==]\n");
    ASSERT_EQ(lits.size(), 2U);
    EXPECT_EQ(lits[0].text, "first"); // the leading newline is dropped
    EXPECT_EQ(lits[1].text, "a]]b");
    // Indexing with a bracket is not a long string.
    EXPECT_EQ(texts("t[1] = x[\"k\"]"), std::vector<std::string>{"k"});
}

TEST(LuaRefs, RequireForms) {
    const auto lits = lua_string_literals(
        "local u = require(\"lib.util\")\nlocal v = require 'lib/vec'\n"
        "local w = require  (  [[lib.w]] )\nlocal s = 'plain.lua'\nnotrequire('x')\n");
    ASSERT_EQ(lits.size(), 5U);
    EXPECT_TRUE(lits[0].required);
    EXPECT_EQ(lits[0].text, "lib.util");
    EXPECT_TRUE(lits[1].required);
    EXPECT_TRUE(lits[2].required);
    EXPECT_FALSE(lits[3].required);
    EXPECT_FALSE(lits[4].required);
    EXPECT_EQ(lits[3].line, 4U);
}

TEST(LuaRefs, MalformedInputEndsTheScan) {
    EXPECT_EQ(texts("local a = 'ok' local b = \"unterminated\nlocal c = 'never'"),
              std::vector<std::string>{"ok"});
    EXPECT_TRUE(texts("--[[ open forever").empty());
    EXPECT_TRUE(texts("").empty());
}

} // namespace
