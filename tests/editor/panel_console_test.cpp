// Console panel model (ROADMAP E8.3): tails the engine log ring
// incrementally, filters by level and text, and keeps the newest line for
// the status bar.

#ifdef ENGINE_EDITOR

#include <string>

#include <gtest/gtest.h>

#include "editor/panels/console.hpp"
#include "engine/core/log.hpp"

namespace {

namespace p = engine::editor::panels;
using engine::core::LogLevel;
using engine::core::LogRing;

TEST(EditorConsole, TailsTheRingIncrementally) {
    LogRing ring(64);
    p::ConsoleModel m;
    ring.push(LogLevel::info, "editor", "first");
    ring.push(LogLevel::warn, "nav", "second");
    EXPECT_EQ(m.poll(ring), 2U);
    EXPECT_EQ(m.poll(ring), 0U) << "nothing new";
    ring.push(LogLevel::error, "vulkan", "third");
    EXPECT_EQ(m.poll(ring), 1U);
    ASSERT_EQ(m.lines().size(), 3U);
    EXPECT_EQ(m.lines()[2].message, "third");
}

TEST(EditorConsole, LevelAndTextFilters) {
    LogRing ring(64);
    p::ConsoleModel m;
    ring.push(LogLevel::debug, "rhi", "pipeline cache hit");
    ring.push(LogLevel::info, "editor", "saved scene: a.json");
    ring.push(LogLevel::warn, "nav", "agent stuck");
    ring.push(LogLevel::error, "remote", "bind failed");
    (void)m.poll(ring);
    const auto visible = [&] {
        int n = 0;
        for (const auto& l : m.lines()) n += m.visible(l) ? 1 : 0;
        return n;
    };
    EXPECT_EQ(visible(), 3) << "debug hidden by default";
    m.show[static_cast<std::size_t>(LogLevel::debug)] = true;
    EXPECT_EQ(visible(), 4);
    m.show[static_cast<std::size_t>(LogLevel::info)] = false;
    EXPECT_EQ(visible(), 3);
    m.search = "NAV"; // category match, case-insensitive
    EXPECT_EQ(visible(), 1);
    m.search = "scene";
    EXPECT_EQ(visible(), 0) << "info is hidden";
    const auto counts = m.counts();
    EXPECT_EQ(counts[static_cast<std::size_t>(LogLevel::warn)], 1U);
    EXPECT_EQ(counts[static_cast<std::size_t>(LogLevel::error)], 1U);
}

TEST(EditorConsole, LastLineAndClear) {
    LogRing ring(64);
    p::ConsoleModel m;
    EXPECT_EQ(m.last(), nullptr);
    ring.push(LogLevel::info, "a", "one");
    ring.push(LogLevel::debug, "a", "noise");
    (void)m.poll(ring);
    ASSERT_NE(m.last(), nullptr);
    EXPECT_EQ(m.last()->message, "one") << "debug lines do not reach the status bar";
    EXPECT_EQ(m.last(LogLevel::trace)->message, "noise");
    m.clear();
    EXPECT_TRUE(m.lines().empty());
    EXPECT_EQ(m.poll(ring), 0U) << "clear keeps the cursor: old lines do not come back";
    ring.push(LogLevel::info, "a", "two");
    EXPECT_EQ(m.poll(ring), 1U);
}

TEST(EditorConsole, BoundedHistory) {
    LogRing ring(8192);
    p::ConsoleModel m;
    for (std::size_t i = 0; i < p::ConsoleModel::k_max_lines + 100; ++i) {
        ring.push(LogLevel::info, "x", std::to_string(i));
    }
    while (m.poll(ring, 1000) > 0) {
    }
    ASSERT_EQ(m.lines().size(), p::ConsoleModel::k_max_lines);
    EXPECT_EQ(m.lines().front().message, "100") << "oldest dropped";
}

TEST(EditorConsole, CopyFormat) {
    engine::core::LogLine l;
    l.time_s = 1.5;
    l.level = LogLevel::warn;
    l.category = "nav";
    l.message = "agent stuck";
    EXPECT_EQ(p::format_log_line(l), "[    1.500] [warn] [nav] agent stuck");
}

} // namespace

#endif // ENGINE_EDITOR
