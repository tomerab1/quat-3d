// The component registry (ROADMAP S9): a new codec component is one file that
// registers itself. These tests prove the codec, its listings and its error
// messages need no edit to a central list: a component defined only in this
// file works everywhere the codec is used, and the engine's own table is
// generated from src/scene/codec/.

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;

// A component the engine has never heard of.
struct Beacon {
    float range = 5.0F;
    bool  lit = true;
};

[[nodiscard]] s::ComponentCodec beacon_codec(std::int32_t order = s::ComponentCodec::k_default_order) {
    s::ComponentCodec codec{.name = "test_beacon", .summary = "A test-only beacon", .order = order};
    codec.encode = [](const entt::registry& r, entt::entity e, const s::EncodeOptions&) {
        const Beacon& b = r.get<Beacon>(e);
        return json{{"range", b.range}, {"lit", b.lit}};
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, engine::core::Error> {
        Beacon b = r.all_of<Beacon>(e) ? r.get<Beacon>(e) : Beacon{};
        engine::core::JsonFields f(v, "test_beacon", strict);
        f.number("range", b.range, 0.0F, 100.0F).boolean("lit", b.lit);
        if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
        r.emplace_or_replace<Beacon>(e, b);
        return {};
    };
    return s::typed_codec<Beacon>(std::move(codec));
}

// Registers the beacon in the process-wide registry for one test.
class ScopedBeacon {
public:
    ScopedBeacon() { ok_ = s::component_registry().add(beacon_codec()).has_value(); }
    ~ScopedBeacon() { (void)s::component_registry().remove("test_beacon"); }
    ScopedBeacon(const ScopedBeacon&) = delete;
    ScopedBeacon& operator=(const ScopedBeacon&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

TEST(ComponentRegistry, ANewComponentWorksThroughTheWholeCodec) {
    const ScopedBeacon beacon;
    ASSERT_TRUE(beacon.ok());
    entt::registry r;
    const entt::entity e = r.create();

    const s::ComponentInfo* info = s::find_component_info("test_beacon");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->summary, "A test-only beacon");
    EXPECT_TRUE(info->addable);
    EXPECT_EQ(s::component_infos().back().name, "test_beacon"); // games sort last

    ASSERT_OK(s::apply_component(r, e, "test_beacon", {{"range", 12.5}}, true));
    ASSERT_OK(s::apply_component(r, e, "transform", {{"position", {1, 2, 3}}}, true));
    EXPECT_FLOAT_EQ(r.get<Beacon>(e).range, 12.5F);

    const json all = s::encode_components(r, e);
    EXPECT_EQ(all["test_beacon"], (json{{"range", 12.5}, {"lit", true}}));
    const std::vector<std::string> only{"test_beacon"};
    EXPECT_EQ(s::encode_components(r, e, {.only = only}).size(), 1U);
    const auto names = s::component_names(r, e);
    EXPECT_EQ(names, (std::vector<std::string_view>{"transform", "test_beacon"}));

    // Strict decoding and range checks come from the component's own codec.
    auto bad = s::apply_component(r, e, "test_beacon", {{"range", 1000}}, true);
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("test_beacon.range"), std::string::npos) << bad.error().message;
    // ...and the unknown-component error lists it among the known ones.
    auto unknown = s::apply_component(r, e, "no_such", json::object(), true);
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().message.find("test_beacon"), std::string::npos);

    EXPECT_TRUE(s::remove_component(r, e, "test_beacon"));
    EXPECT_FALSE(r.all_of<Beacon>(e));
    EXPECT_FALSE(s::remove_component(r, e, "test_beacon"));
}

TEST(ComponentRegistry, UnregisteringRemovesItEverywhere) {
    {
        const ScopedBeacon beacon;
        ASSERT_TRUE(beacon.ok());
    }
    EXPECT_EQ(s::find_component_info("test_beacon"), nullptr);
    entt::registry r;
    EXPECT_FALSE(s::apply_component(r, r.create(), "test_beacon", json::object(), false));
}

TEST(ComponentRegistry, RefusesInvalidRegistrations) {
    s::ComponentRegistry registry;
    ASSERT_OK(registry.add(beacon_codec()));
    EXPECT_FALSE(registry.add(beacon_codec())); // duplicate
    for (const char* bad : {"", "Beacon", "2beacon", "bea-con"}) {
        s::ComponentCodec c = beacon_codec();
        c.name = bad;
        EXPECT_FALSE(registry.add(std::move(c))) << bad;
    }
    s::ComponentCodec no_apply = beacon_codec();
    no_apply.name = "no_apply";
    no_apply.apply = nullptr;
    EXPECT_FALSE(registry.add(std::move(no_apply)));
    s::ComponentCodec read_only_addable = beacon_codec();
    read_only_addable.name = "read_only_addable";
    read_only_addable.writable = false;
    EXPECT_FALSE(registry.add(std::move(read_only_addable)));
    EXPECT_EQ(registry.size(), 1U);
}

TEST(ComponentRegistry, OrdersByOrderThenRegistration) {
    s::ComponentRegistry registry;
    const auto named = [](std::string name, std::int32_t order) {
        s::ComponentCodec c = beacon_codec(order);
        c.name = std::move(name);
        return c;
    };
    ASSERT_OK(registry.add(named("c_late", 300)));
    ASSERT_OK(registry.add(named("a_first", 100)));
    ASSERT_OK(registry.add(named("b_tie_one", 200)));
    ASSERT_OK(registry.add(named("b_tie_two", 200)));
    std::vector<std::string_view> order;
    for (const s::ComponentInfo& i : registry.infos()) order.push_back(i.name);
    EXPECT_EQ(order, (std::vector<std::string_view>{"a_first", "b_tie_one", "b_tie_two", "c_late"}));
    EXPECT_TRUE(registry.remove("b_tie_one"));
    EXPECT_EQ(registry.size(), 3U);
    EXPECT_EQ(registry.infos()[1].name, "b_tie_two");
}

// The engine's table is generated from src/scene/codec/: every file there is
// registered, under its file name, and nothing else is.
TEST(EngineComponents, GeneratedFromTheCodecDirectory) {
    std::set<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(QUAT_PROJECT_ROOT) / "src" / "scene" / "codec")) {
        if (entry.path().extension() == ".cpp") files.insert(entry.path().stem().string());
    }
    std::set<std::string> generated;
    for (const std::string_view n : s::engine_component_sources()) generated.emplace(n);
    EXPECT_EQ(files, generated);

    s::ComponentRegistry registry;
    ASSERT_OK(s::register_engine_components(registry));
    std::set<std::string> registered;
    for (const s::ComponentInfo& i : registry.infos()) registered.emplace(i.name);
    EXPECT_EQ(registered, files) << "each src/scene/codec/<name>.cpp registers component <name>";
}

// The canonical order scene files and scripts rely on (component_json.hpp).
TEST(EngineComponents, KeepTheCanonicalOrder) {
    std::vector<std::string_view> order;
    for (const s::ComponentInfo& i : s::component_infos()) order.push_back(i.name);
    const std::vector<std::string_view> expected{
        "transform", "primitive", "material", "instanced_mesh", "scatter", "point_light",
        "dir_light", "camera", "collider", "rigidbody", "character", "joint", "nav_agent",
        "behavior", "patrol", "script", "terrain", "grass", "weather", "particle_emitter",
        "world_clock", "net_replicated", "audio_source", "audio_listener", "animator", "foot_ik",
        "camera_rig", "camera_rig_input", "mesh"};
    EXPECT_EQ(order, expected);
}

} // namespace
