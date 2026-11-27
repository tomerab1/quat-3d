// LogRing: sequence numbers, wraparound and missed counts, filtering,
// snapshot vs incremental tails, thread safety; stderr line parsing.

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "engine/core/log.hpp"

namespace {

using engine::core::LogLevel;
using engine::core::LogQuery;
using engine::core::LogRing;

LogQuery since(std::uint64_t seq, std::size_t limit = 100) {
    LogQuery q;
    q.since_seq = seq;
    q.limit = limit;
    return q;
}

TEST(LogRing, AssignsIncreasingSequenceNumbers) {
    LogRing ring(8);
    EXPECT_EQ(ring.last_seq(), 0U);
    EXPECT_EQ(ring.first_seq(), 0U);
    EXPECT_EQ(ring.push(LogLevel::info, "a", "one"), 1U);
    EXPECT_EQ(ring.push(LogLevel::warn, "b", "two"), 2U);
    EXPECT_EQ(ring.last_seq(), 2U);
    EXPECT_EQ(ring.first_seq(), 1U);

    const auto r = ring.query(since(0));
    ASSERT_EQ(r.lines.size(), 2U);
    EXPECT_EQ(r.lines[0].seq, 1U);
    EXPECT_EQ(r.lines[0].category, "a");
    EXPECT_EQ(r.lines[1].message, "two");
    EXPECT_EQ(r.lines[1].level, LogLevel::warn);
    EXPECT_GE(r.lines[1].time_s, r.lines[0].time_s);
    EXPECT_EQ(r.cursor, 2U);
    EXPECT_FALSE(r.more);
    EXPECT_EQ(r.missed, 0U);
}

TEST(LogRing, EmptyRingQueriesCleanly) {
    LogRing ring(4);
    const auto snap = ring.query(LogQuery{});
    EXPECT_TRUE(snap.lines.empty());
    EXPECT_EQ(snap.cursor, 0U);
    const auto inc = ring.query(since(5));
    EXPECT_TRUE(inc.lines.empty());
}

TEST(LogRing, WrapsAroundAndReportsMissedLines) {
    LogRing ring(4);
    for (int i = 1; i <= 10; ++i) ring.push(LogLevel::info, "t", "line " + std::to_string(i));
    EXPECT_EQ(ring.size(), 4U);
    EXPECT_EQ(ring.first_seq(), 7U);
    EXPECT_EQ(ring.last_seq(), 10U);

    // A reader that last saw seq 3 missed 4, 5 and 6.
    const auto r = ring.query(since(3));
    ASSERT_EQ(r.lines.size(), 4U);
    EXPECT_EQ(r.lines.front().seq, 7U);
    EXPECT_EQ(r.lines.front().message, "line 7");
    EXPECT_EQ(r.lines.back().seq, 10U);
    EXPECT_EQ(r.missed, 3U);

    // Caught up: nothing new, nothing missed.
    const auto none = ring.query(since(10));
    EXPECT_TRUE(none.lines.empty());
    EXPECT_EQ(none.cursor, 10U);
    EXPECT_EQ(none.missed, 0U);

    // Mid-ring cursor.
    const auto tail = ring.query(since(8));
    ASSERT_EQ(tail.lines.size(), 2U);
    EXPECT_EQ(tail.lines[0].seq, 9U);
}

TEST(LogRing, IncrementalReadsPageWithCursor) {
    LogRing ring(64);
    for (int i = 0; i < 25; ++i) ring.push(LogLevel::info, "t", std::to_string(i));
    std::vector<std::uint64_t> seen;
    std::uint64_t cursor = 0;
    for (int pages = 0; pages < 10; ++pages) {
        const auto r = ring.query(since(cursor, 10));
        for (const auto& l : r.lines) seen.push_back(l.seq);
        cursor = r.cursor;
        if (!r.more) break;
    }
    ASSERT_EQ(seen.size(), 25U);
    for (std::size_t i = 0; i < seen.size(); ++i) EXPECT_EQ(seen[i], i + 1);
}

TEST(LogRing, SnapshotReturnsNewestMatchesOldestFirst) {
    LogRing ring(64);
    for (int i = 1; i <= 20; ++i) ring.push(LogLevel::info, "t", std::to_string(i));
    LogQuery q;
    q.limit = 3;
    const auto r = ring.query(q);
    ASSERT_EQ(r.lines.size(), 3U);
    EXPECT_EQ(r.lines[0].message, "18");
    EXPECT_EQ(r.lines[2].message, "20");
    EXPECT_TRUE(r.more);
    EXPECT_EQ(r.cursor, 20U); // continue tailing from the newest line
}

TEST(LogRing, FiltersByLevelCategoryAndText) {
    LogRing ring(64);
    ring.push(LogLevel::debug, "physics", "step");
    ring.push(LogLevel::info, "nav", "baked 12 polygons");
    ring.push(LogLevel::warn, "nav", "agent stuck");
    ring.push(LogLevel::error, "vulkan", "VUID-vkCmdDraw-None-02699");
    ring.push(LogLevel::warn, "stderr", "primitive resolve: no mesh");

    LogQuery q = since(0);
    q.min_level = LogLevel::warn;
    auto r = ring.query(q);
    ASSERT_EQ(r.lines.size(), 3U);
    EXPECT_EQ(r.lines[0].message, "agent stuck");

    q.category = "nav";
    r = ring.query(q);
    ASSERT_EQ(r.lines.size(), 1U);
    EXPECT_EQ(r.lines[0].seq, 3U);

    LogQuery text = since(0);
    text.contains = "VUID";
    r = ring.query(text);
    ASSERT_EQ(r.lines.size(), 1U);
    EXPECT_EQ(r.lines[0].category, "vulkan");
    EXPECT_EQ(r.cursor, 5U) << "filtered-out lines still advance the cursor";
}

TEST(LogRing, CursorFromAnEarlierProcessRestartsAtTheOldestLine) {
    LogRing ring(8);
    ring.push(LogLevel::info, "t", "a");
    ring.push(LogLevel::info, "t", "b");
    const auto r = ring.query(since(5000));
    ASSERT_EQ(r.lines.size(), 2U);
    EXPECT_EQ(r.lines[0].message, "a");
}

TEST(LogRing, ClipsOverlongMessagesOnACharacterBoundary) {
    LogRing ring(2);
    // 3-byte UTF-8 characters, so the cut falls mid-sequence unless adjusted.
    std::string text;
    while (text.size() < LogRing::max_message_bytes + 10) text += "\xE2\x82\xAC"; // €
    ring.push(LogLevel::info, "t", text);
    const auto r = ring.query(since(0));
    ASSERT_EQ(r.lines.size(), 1U);
    const std::string& m = r.lines[0].message;
    EXPECT_LE(m.size(), LogRing::max_message_bytes);
    EXPECT_EQ(m.size() % 3, 0U) << "cut inside a UTF-8 sequence";
}

TEST(LogRing, ConcurrentPushesKeepSequencesUniqueAndContiguous) {
    constexpr int k_threads = 8;
    constexpr int k_each = 2000;
    LogRing ring(k_threads * k_each);
    std::vector<std::thread> threads;
    for (int t = 0; t < k_threads; ++t) {
        threads.emplace_back([&ring, t] {
            for (int i = 0; i < k_each; ++i) {
                ring.push(LogLevel::info, "thread", std::to_string(t) + ":" + std::to_string(i));
            }
        });
    }
    // A concurrent reader must always see a consistent ring.
    std::thread reader([&ring] {
        for (int i = 0; i < 200; ++i) {
            const auto r = ring.query(since(0, 1000));
            for (std::size_t j = 1; j < r.lines.size(); ++j) {
                ASSERT_EQ(r.lines[j].seq, r.lines[j - 1].seq + 1);
            }
        }
    });
    for (auto& th : threads) th.join();
    reader.join();

    EXPECT_EQ(ring.last_seq(), static_cast<std::uint64_t>(k_threads * k_each));
    const auto all = ring.query(since(0, k_threads * k_each));
    ASSERT_EQ(all.lines.size(), static_cast<std::size_t>(k_threads * k_each));
    for (std::size_t i = 0; i < all.lines.size(); ++i) EXPECT_EQ(all.lines[i].seq, i + 1);
    // Per thread, lines keep their order.
    std::vector<int> next(k_threads, 0);
    for (const auto& l : all.lines) {
        const auto colon = l.message.find(':');
        const int t = std::stoi(l.message.substr(0, colon));
        EXPECT_EQ(std::stoi(l.message.substr(colon + 1)), next[static_cast<std::size_t>(t)]++);
    }
}

TEST(LogLevelNames, ParseAndPrint) {
    using engine::core::parse_log_level;
    EXPECT_EQ(parse_log_level("WARN"), LogLevel::warn);
    EXPECT_EQ(parse_log_level("warning"), LogLevel::warn);
    EXPECT_EQ(parse_log_level("fatal"), LogLevel::error);
    EXPECT_EQ(parse_log_level("Debug"), LogLevel::debug);
    EXPECT_FALSE(parse_log_level("loud").has_value());
    EXPECT_EQ(engine::core::to_string(LogLevel::error), "error");
}

TEST(StderrLineParsing, InterpretsLeadingTags) {
    using engine::core::parse_stderr_line;
    auto p = parse_stderr_line("[warn] primitive resolve: failed\n");
    EXPECT_EQ(p.level, LogLevel::warn);
    EXPECT_EQ(p.category, "stderr");
    EXPECT_EQ(p.message, "primitive resolve: failed");

    p = parse_stderr_line("[nav] no navmesh for the agents: baking one");
    EXPECT_EQ(p.level, LogLevel::info);
    EXPECT_EQ(p.category, "nav");
    EXPECT_EQ(p.message, "no navmesh for the agents: baking one");

    p = parse_stderr_line("[vulkan][ERROR] Validation Error: [ VUID-x ]");
    EXPECT_EQ(p.level, LogLevel::error);
    EXPECT_EQ(p.category, "vulkan");
    EXPECT_EQ(p.message, "Validation Error: [ VUID-x ]");

    p = parse_stderr_line("[fatal] gbuffer pass: out of memory\r\n");
    EXPECT_EQ(p.level, LogLevel::error);
    EXPECT_EQ(p.message, "gbuffer pass: out of memory");

    p = parse_stderr_line("plain text [not a tag]");
    EXPECT_EQ(p.category, "stderr");
    EXPECT_EQ(p.message, "plain text [not a tag]");

    p = parse_stderr_line("[ spaced tag ] kept");
    EXPECT_EQ(p.category, "stderr");
    EXPECT_EQ(p.message, "[ spaced tag ] kept");
}

// counts() is kept on push (no copy of the ring) and covers only the retained
// lines, whatever the ring's capacity.
TEST(LogRing, CountsRetainedWarningsAndErrors) {
    LogRing ring(4);
    EXPECT_EQ(ring.counts().warnings, 0U);
    EXPECT_EQ(ring.counts().last_error_seq, 0U);

    (void)ring.push(LogLevel::warn, "a", "w1");  // seq 1
    (void)ring.push(LogLevel::error, "a", "e1"); // seq 2
    (void)ring.push(LogLevel::info, "a", "i1");  // seq 3
    (void)ring.push(LogLevel::error, "a", "e2"); // seq 4
    auto c = ring.counts();
    EXPECT_EQ(c.warnings, 1U);
    EXPECT_EQ(c.errors, 2U);
    EXPECT_EQ(c.last_error_seq, 4U);

    (void)ring.push(LogLevel::info, "a", "i2"); // evicts w1
    (void)ring.push(LogLevel::info, "a", "i3"); // evicts e1
    c = ring.counts();
    EXPECT_EQ(c.warnings, 0U);
    EXPECT_EQ(c.errors, 1U);
    EXPECT_EQ(c.last_error_seq, 4U);

    (void)ring.push(LogLevel::debug, "a", "d1"); // evicts i1
    (void)ring.push(LogLevel::warn, "a", "w2");  // evicts e2: no error retained
    c = ring.counts();
    EXPECT_EQ(c.warnings, 1U);
    EXPECT_EQ(c.errors, 0U);
    EXPECT_EQ(c.last_error_seq, 0U);

    // Agrees with a full query of the ring.
    LogQuery q = since(0, ring.capacity());
    q.min_level = LogLevel::warn;
    EXPECT_EQ(ring.query(q).lines.size(), c.warnings + c.errors);
}

} // namespace
