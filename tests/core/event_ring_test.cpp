// EventRing (A10.1): sequence numbers, incremental reads, overwrite and
// producer drops reported as gaps.

#include <gtest/gtest.h>

#include "engine/core/event_ring.hpp"

namespace {

using engine::core::Event;
using engine::core::EventKind;
using engine::core::EventRing;

Event contact(std::uint32_t a, std::uint32_t b) {
    Event e;
    e.kind = EventKind::contact_added;
    e.entity_a = a;
    e.entity_b = b;
    return e;
}

TEST(EventRing, TailsIncrementallyFromACursor) {
    EventRing ring(16);
    EXPECT_EQ(ring.push(contact(1, 2)), 1U);
    EXPECT_EQ(ring.push(contact(3, 4)), 2U);
    auto q = ring.query(0, 100);
    ASSERT_EQ(q.events.size(), 2U);
    EXPECT_EQ(q.events[0].entity_a, 1U);
    EXPECT_EQ(q.cursor, 2U);
    EXPECT_FALSE(q.more);
    EXPECT_EQ(q.dropped, 0U);

    ring.push(contact(5, 6));
    q = ring.query(q.cursor, 100);
    ASSERT_EQ(q.events.size(), 1U);
    EXPECT_EQ(q.events[0].seq, 3U);
    EXPECT_EQ(ring.query(q.cursor, 100).events.size(), 0U);
}

TEST(EventRing, PagesWithMax) {
    EventRing ring(16);
    for (std::uint32_t i = 0; i < 5; ++i) ring.push(contact(i, i));
    auto q = ring.query(0, 2);
    ASSERT_EQ(q.events.size(), 2U);
    EXPECT_TRUE(q.more);
    EXPECT_EQ(q.cursor, 2U);
    q = ring.query(q.cursor, 10);
    EXPECT_EQ(q.events.size(), 3U);
    EXPECT_EQ(q.events.front().seq, 3U);
    EXPECT_FALSE(q.more);
}

TEST(EventRing, OverwrittenEventsAreReportedAsDropped) {
    EventRing ring(4);
    for (std::uint32_t i = 0; i < 10; ++i) ring.push(contact(i, i));
    const auto q = ring.query(0, 100);
    ASSERT_EQ(q.events.size(), 4U);
    EXPECT_EQ(q.events.front().seq, 7U);
    EXPECT_EQ(q.oldest_seq, 7U);
    EXPECT_EQ(q.dropped, 6U);
    EXPECT_EQ(q.cursor, 10U);
}

TEST(EventRing, ProducerDropsLeaveAGap) {
    EventRing ring(16);
    ring.push(contact(1, 1));
    ring.drop(5); // a physics step overflowed its buffer
    ring.push(contact(2, 2));
    const auto q = ring.query(1, 100);
    ASSERT_EQ(q.events.size(), 1U);
    EXPECT_EQ(q.events[0].seq, 7U);
    EXPECT_EQ(q.dropped, 5U);
    EXPECT_EQ(q.last_seq, 7U);
}

TEST(EventRing, ACursorFromAnEarlierRunRestartsAtTheOldest) {
    EventRing ring(16);
    ring.push(contact(1, 1));
    const auto q = ring.query(500, 100);
    ASSERT_EQ(q.events.size(), 1U);
    EXPECT_EQ(q.cursor, 1U);
}

TEST(EventRing, KindsHaveStableNames) {
    EXPECT_EQ(engine::core::to_string(EventKind::contact_added), "contact_added");
    EXPECT_EQ(engine::core::to_string(EventKind::contact_removed), "contact_removed");
    EXPECT_EQ(engine::core::to_string(EventKind::trigger_enter), "trigger_enter");
    EXPECT_EQ(engine::core::to_string(EventKind::trigger_exit), "trigger_exit");
}

} // namespace
