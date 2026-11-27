// DeferredReleaseQueue (R2): objects replaced at frame N are released only
// once frame N + frames_in_flight begins, in push order; flush() releases
// everything (device idle).

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/core/deferred_release_queue.hpp"

namespace {

using engine::core::DeferredReleaseQueue;

TEST(DeferredReleaseQueue, ReleasesAfterFramesInFlight) {
    std::vector<std::string> log;
    DeferredReleaseQueue queue(2);
    queue.push(10, [&] { log.push_back("a@10"); });

    EXPECT_EQ(queue.collect(10), 0U); // still recorded/in flight
    EXPECT_EQ(queue.collect(11), 0U); // frame 10 may still execute on the GPU
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(queue.collect(12), 1U); // frame 12's fence wait proved frame 10 done
    EXPECT_EQ(log, (std::vector<std::string>{"a@10"}));
    EXPECT_EQ(queue.size(), 0U);
}

TEST(DeferredReleaseQueue, KeepsPushOrderAndOnlyReleasesExpired) {
    std::vector<std::string> log;
    DeferredReleaseQueue queue(2);
    queue.push(5, [&] { log.push_back("p5a"); });
    queue.push(7, [&] { log.push_back("p7"); });
    queue.push(5, [&] { log.push_back("p5b"); });
    queue.push(6, [&] { log.push_back("p6"); });

    EXPECT_EQ(queue.collect(8), 3U); // 5, 5, 6 expire; 7 must survive
    EXPECT_EQ(log, (std::vector<std::string>{"p5a", "p5b", "p6"}));
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_EQ(queue.collect(8), 0U);
    EXPECT_EQ(queue.collect(9), 1U);
    EXPECT_EQ(log.back(), "p7");
}

TEST(DeferredReleaseQueue, HonoursFramesInFlight) {
    int released = 0;
    DeferredReleaseQueue queue(3);
    queue.push(0, [&] { ++released; });
    queue.collect(2);
    EXPECT_EQ(released, 0);
    queue.collect(3);
    EXPECT_EQ(released, 1);
}

TEST(DeferredReleaseQueue, FlushAndDestructorReleaseEverything) {
    int released = 0;
    {
        DeferredReleaseQueue queue(2);
        queue.push(100, [&] { ++released; });
        queue.push(101, [&] { ++released; });
        queue.flush();
        EXPECT_EQ(released, 2);
        queue.push(102, [&] { ++released; });
    } // destructor flushes the rest
    EXPECT_EQ(released, 3);
}

TEST(DeferredReleaseQueue, MoveTransfersPendingReleases) {
    int released = 0;
    DeferredReleaseQueue a(2);
    a.push(1, [&] { ++released; });
    DeferredReleaseQueue b = std::move(a);
    EXPECT_EQ(b.size(), 1U);
    b.collect(3);
    EXPECT_EQ(released, 1);
}

} // namespace
