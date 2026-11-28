// The JSON Schema subset game-module methods declare (ROADMAP G17.1):
// unsupported keywords are refused at registration, values are validated
// with errors that name their path.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/game/json_schema.hpp"
#include "support/expect_ok.hpp"

namespace {

using nlohmann::json;
using engine::game::check_schema;
using engine::game::validate_json;

std::string error_of(const json& schema, const json& value) {
    auto ok = validate_json(schema, value);
    return ok ? std::string() : ok.error().message;
}

TEST(JsonSchema, RefusesWhatItCannotEnforce) {
    EXPECT_OK(check_schema(json::object()));
    EXPECT_OK(check_schema(json::parse(R"({"type": ["object", "null"], "properties": {
        "a": {"anyOf": [{"type": "string", "maxLength": 3}, {"const": 1}]}}})")));
    for (const char* bad : {R"({"pattern": "x"})", R"({"type": "decimal"})",
                            R"({"properties": {"a": {"format": "email"}}})",
                            R"({"additionalProperties": {"type": "string"}})", R"({"minItems": -1})",
                            R"({"required": [1]})", R"([1])"}) {
        EXPECT_FALSE(check_schema(json::parse(bad))) << bad;
    }
}

TEST(JsonSchema, ValidatesWithPaths) {
    const json schema = json::parse(R"({
        "type": "object", "required": ["size"], "additionalProperties": false,
        "properties": {
            "size": {"type": "array", "items": {"type": "number", "maximum": 64}, "minItems": 2, "maxItems": 2},
            "style": {"enum": ["stone", "wood"]},
            "name": {"type": "string", "minLength": 1, "maxLength": 4},
            "count": {"type": "integer", "exclusiveMinimum": 0}
        }})");
    EXPECT_EQ(error_of(schema, {{"size", {1, 2}}, {"style", "wood"}, {"name", "ab"}, {"count", 2.0}}), "");
    EXPECT_EQ(error_of(schema, {{"size", {1, 65}}}), "params.size[1]: expected a number <= 64");
    EXPECT_EQ(error_of(schema, {{"size", {1}}}), "params.size: expected at least 2 items");
    EXPECT_EQ(error_of(schema, json::object()), "params.size: required");
    EXPECT_NE(error_of(schema, {{"size", {1, 2}}, {"extra", 1}}).find("params.extra: unknown field (known:"),
              std::string::npos);
    EXPECT_NE(error_of(schema, {{"size", {1, 2}}, {"style", "glass"}}).find("params.style: expected one of"),
              std::string::npos);
    EXPECT_EQ(error_of(schema, {{"size", {1, 2}}, {"name", "héllo"}}), "params.name: expected at most 4 characters");
    EXPECT_EQ(error_of(schema, {{"size", {1, 2}}, {"count", 1.5}}), "params.count: expected integer");
    EXPECT_EQ(error_of(schema, {{"size", {1, 2}}, {"count", 0}}), "params.count: expected a number > 0");
    EXPECT_EQ(error_of(schema, json::array()), "params: expected object");
}

} // namespace
