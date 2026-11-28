// JsonFields' loose scalar readers: untrusted values convert only when they
// have the expected JSON type (CLAUDE.md: never call get<> on request data).

#include <cstdint>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"

namespace {

using engine::core::JsonFields;
using Json = nlohmann::json;

TEST(JsonFieldsScalars, ReadBoolAcceptsOnlyBooleans) {
    bool b = false;
    EXPECT_TRUE(JsonFields::read_bool(Json(true), b));
    EXPECT_TRUE(b);
    EXPECT_TRUE(JsonFields::read_bool(Json(false), b));
    EXPECT_FALSE(b);
    b = true;
    for (const Json& v : {Json(1), Json(0), Json("true"), Json(nullptr), Json::array({true}),
                          Json::object()}) {
        EXPECT_FALSE(JsonFields::read_bool(v, b)) << v.dump();
        EXPECT_TRUE(b) << "a rejected value leaves the output alone: " << v.dump();
    }
}

TEST(JsonFieldsScalars, ReadFloatAndIntRejectOtherTypes) {
    float         f = 0.0F;
    std::int64_t  i = 0;
    EXPECT_FALSE(JsonFields::read_float(Json(true), f));
    EXPECT_FALSE(JsonFields::read_int(Json(true), i));
    EXPECT_TRUE(JsonFields::read_float(Json(2.5), f));
    EXPECT_FLOAT_EQ(f, 2.5F);
    EXPECT_TRUE(JsonFields::read_int(Json(7), i));
    EXPECT_EQ(i, 7);
    EXPECT_FALSE(JsonFields::read_int(Json(7.5), i));
}

} // namespace
