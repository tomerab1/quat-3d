// game.json format 2 (ROADMAP G17.1): native modules, the MCP folder and the
// engine version range; format 1 files stay valid and may not use them.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/project/game_project.hpp"
#include "support/expect_ok.hpp"

namespace {

using nlohmann::json;
namespace project = engine::project;

json format2() {
    return json::parse(R"({
        "format": 2,
        "name": "Cozy Builder",
        "id": "dev.quat3d.cozy-builder",
        "version": "0.0.1",
        "start_scene": "assets/scenes/meadow.json",
        "modules": ["cozy_builder", "cozy_extras"],
        "mcp": "mcp",
        "engine": ">=0.1,<0.2"
    })");
}

std::string error_of(const json& doc) {
    auto p = project::parse_game_project(doc);
    return p ? std::string() : p.error().message;
}

TEST(GameProjectFormat2, ParsesModulesMcpAndEngine) {
    auto p = project::parse_game_project(format2());
    ASSERT_OK(p);
    EXPECT_EQ(p->format, 2);
    EXPECT_EQ(p->modules, (std::vector<std::string>{"cozy_builder", "cozy_extras"}));
    EXPECT_EQ(p->mcp, "mcp");
    EXPECT_EQ(p->engine, ">=0.1,<0.2");
    // Round trip through the writer.
    auto again = project::parse_game_project(project::game_project_json(*p));
    ASSERT_OK(again);
    EXPECT_EQ(again->modules, p->modules);
    EXPECT_EQ(again->engine, p->engine);
}

TEST(GameProjectFormat2, FormatOneMayNotUseTheNewKeys) {
    for (const char* key : {"modules", "mcp", "engine"}) {
        json doc = format2();
        doc["format"] = 1;
        for (const char* other : {"modules", "mcp", "engine"}) {
            if (std::string(other) != key) doc.erase(other);
        }
        EXPECT_NE(error_of(doc).find(std::string(key) + ": needs \"format\": 2"), std::string::npos)
            << error_of(doc);
    }
    json plain = format2();
    plain["format"] = 1;
    plain.erase("modules");
    plain.erase("mcp");
    plain.erase("engine");
    EXPECT_OK(project::parse_game_project(plain));
}

TEST(GameProjectFormat2, ValidatesEachKey) {
    json doc = format2();
    doc["modules"] = json::array({"Cozy"});
    EXPECT_NE(error_of(doc).find("modules: expected module names"), std::string::npos) << error_of(doc);
    doc["modules"] = json::array({"a", "a"});
    EXPECT_NE(error_of(doc).find("'a' is listed twice"), std::string::npos) << error_of(doc);
    doc = format2();
    doc["mcp"] = "../outside";
    EXPECT_NE(error_of(doc).find("mcp:"), std::string::npos) << error_of(doc);
    doc = format2();
    doc["engine"] = "^0.4";
    EXPECT_NE(error_of(doc).find("engine:"), std::string::npos) << error_of(doc);
}

TEST(EngineRange, ComparatorsMustAllHold) {
    EXPECT_OK(project::check_engine_range(">=0.4,<0.5", 0, 4, 2));
    EXPECT_FALSE(project::check_engine_range(">=0.4,<0.5", 0, 5, 0));
    EXPECT_FALSE(project::check_engine_range(">=0.4,<0.5", 0, 3, 9));
    EXPECT_OK(project::check_engine_range("0.1.0", 0, 1, 0));
    EXPECT_OK(project::check_engine_range("=0.1", 0, 1, 0)); // PATCH defaults to 0
    EXPECT_FALSE(project::check_engine_range("=0.1", 0, 1, 1));
    EXPECT_OK(project::check_engine_range(">0.1.0, <=1.0", 1, 0, 0));
    auto e = project::check_engine_range(">=0.4,<0.5", 0, 1, 0);
    ASSERT_FALSE(e);
    EXPECT_NE(e.error().message.find("needs engine >=0.4,<0.5; this is quat-3d 0.1.0"),
              std::string::npos)
        << e.error().message;
    for (const char* bad : {"", "0", ">=x.1", ">=0.4,", "~0.4", "0.4.1.2", "=>0.4"}) {
        EXPECT_FALSE(project::validate_engine_range(bad)) << bad;
    }
}

} // namespace
