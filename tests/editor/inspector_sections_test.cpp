// Inspector sections (ROADMAP S9): one file per component under
// src/editor/inspectors/, registered from a table the build generates from
// that directory, drawn in a fixed order.

#ifdef ENGINE_EDITOR

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "editor/inspectors/inspect.hpp"

namespace {

namespace ed = engine::editor;

TEST(InspectorSections, GeneratedFromTheInspectorsDirectory) {
    std::set<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(QUAT_PROJECT_ROOT) / "src" / "editor" / "inspectors")) {
        if (entry.path().extension() == ".cpp") files.insert(entry.path().stem().string());
    }
    std::set<std::string> generated;
    for (const std::string_view n : ed::engine_inspector_sources()) generated.emplace(n);
    EXPECT_EQ(files, generated);

    std::set<std::string> registered;
    for (const ed::InspectorEntry& e : ed::inspector_registry().entries()) {
        EXPECT_TRUE(e.draw) << e.name;
        registered.emplace(e.name);
    }
    EXPECT_EQ(registered, files) << "each inspectors/<name>.cpp registers section <name>";
}

// The order the Inspector has always drawn its sections in.
TEST(InspectorSections, KeepTheirOrder) {
    std::vector<std::string_view> order;
    for (const ed::InspectorEntry& e : ed::inspector_registry().entries()) order.push_back(e.name);
    const std::vector<std::string_view> expected{
        "transform", "primitive", "mesh_renderer", "material", "dir_light", "point_light",
        "camera", "camera_rig", "camera_rig_input", "animator", "foot_ik", "terrain", "grass",
        "particle_emitter", "world_clock", "instanced_mesh", "scatter", "nav_agent", "behavior",
        "patrol", "script", "collider", "rigidbody", "joint", "audio_source", "audio_listener",
        "field_table"}; // game-module components (G17.1), after the engine's
    EXPECT_EQ(order, expected);
}

TEST(InspectorSections, ANewSectionSortsByOrder) {
    ed::InspectorRegistry registry;
    registry.add({.name = "late", .order = 300, .draw = {}});
    registry.add({.name = "early", .order = 100, .draw = {}});
    registry.add({.name = "tie", .order = 300, .draw = {}});
    std::vector<std::string_view> order;
    for (const ed::InspectorEntry& e : registry.entries()) order.push_back(e.name);
    EXPECT_EQ(order, (std::vector<std::string_view>{"early", "late", "tie"}));
}

} // namespace

#endif // ENGINE_EDITOR
