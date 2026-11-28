// Inclusive and self time (R4 review): a zone's self time excludes the zones
// nested inside it, the `cost` ordering ranks by it, and the per-thread name
// cache resolves a pass name without interning it again.

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

#include "engine/core/frame_profiler.hpp"
#include "engine/core/profile_names.hpp"

namespace {

using engine::core::FrameProfiler;
using engine::core::ProfileCategory;
using engine::core::ProfileNameCache;
using engine::core::ProfileScope;
using engine::core::ZoneSort;
using engine::core::ZoneSummary;

void spin_for_us(int us) {
    const std::uint64_t until =
        engine::core::profile_now_ns() + static_cast<std::uint64_t>(us) * 1000;
    while (engine::core::profile_now_ns() < until) {
    }
}

[[nodiscard]] const ZoneSummary* find(const std::vector<ZoneSummary>& zones, std::string_view n) {
    const auto it = std::ranges::find_if(zones, [n](const ZoneSummary& z) { return z.name == n; });
    return it == zones.end() ? nullptr : &*it;
}

// Unbinds on every exit, a failed ASSERT included.
struct Bound {
    FrameProfiler& p;
    explicit Bound(FrameProfiler& target) : p(target) { p.bind_current_thread(); }
    ~Bound() { p.unbind_current_thread(); }
    Bound(const Bound&) = delete;
    Bound& operator=(const Bound&) = delete;
};

TEST(ProfileSelfTime, ParentSelfTimeExcludesItsChildren) {
    FrameProfiler profiler(FrameProfiler::Config{.window = 4, .scope_capacity = 64});
    const Bound bound(profiler);
    const auto parent = engine::core::intern_profile_name("self.parent", ProfileCategory::system);
    const auto child = engine::core::intern_profile_name("self.child");
    const auto grandchild = engine::core::intern_profile_name("self.grandchild");
    {
        const ProfileScope p(parent);
        spin_for_us(500); // parent's own work
        for (int i = 0; i < 2; ++i) {
            const ProfileScope c(child);
            spin_for_us(1000);
            const ProfileScope g(grandchild);
            spin_for_us(1000);
        }
    }
    profiler.end_frame();

    const std::vector<ZoneSummary> zones = profiler.zones();
    const ZoneSummary* p = find(zones, "self.parent");
    const ZoneSummary* c = find(zones, "self.child");
    const ZoneSummary* g = find(zones, "self.grandchild");
    ASSERT_TRUE(p && c && g);
    EXPECT_EQ(c->calls, 2U);
    // Leaves: self == inclusive.
    EXPECT_FLOAT_EQ(g->cpu_self.avg, g->cpu.avg);
    // Each level's self time is its inclusive time minus its direct
    // children's inclusive time (both child calls summed).
    EXPECT_NEAR(c->cpu_self.avg, c->cpu.avg - g->cpu.avg, 1e-3F);
    EXPECT_NEAR(p->cpu_self.avg, p->cpu.avg - c->cpu.avg, 1e-3F);
    EXPECT_GE(p->cpu_self.avg, 0.45F); // ~0.5 ms of its own
    EXPECT_LT(p->cpu_self.avg, p->cpu.avg * 0.5F);
    EXPECT_GE(c->cpu_self.avg, 1.9F); // 2 x ~1 ms

    // `cost` ranks by self time: the child (2 ms own) and grandchild (2 ms)
    // come before the parent wrapper (0.5 ms own, 4.5 ms inclusive).
    std::vector<ZoneSummary> by_cost = zones;
    std::erase_if(by_cost, [](const ZoneSummary& z) { return !z.name.starts_with("self."); });
    engine::core::sort_zones(by_cost, ZoneSort::cost);
    ASSERT_EQ(by_cost.size(), 3U);
    EXPECT_EQ(by_cost.back().name, "self.parent");
    std::vector<ZoneSummary> by_cpu = by_cost;
    engine::core::sort_zones(by_cpu, ZoneSort::cpu_avg);
    EXPECT_EQ(by_cpu.front().name, "self.parent") << "the inclusive order is still available";
}

TEST(ProfileSelfTime, SiblingsDoNotLeakIntoEachOthersParents) {
    FrameProfiler profiler(FrameProfiler::Config{.window = 4, .scope_capacity = 64});
    const Bound bound(profiler);
    const auto a = engine::core::intern_profile_name("self.sibling_a");
    const auto b = engine::core::intern_profile_name("self.sibling_b");
    const auto leaf = engine::core::intern_profile_name("self.sibling_leaf");
    {
        const ProfileScope s(a);
        const ProfileScope l(leaf);
        spin_for_us(1500);
    }
    {
        const ProfileScope s(b); // no children: its self time is all of it
        spin_for_us(800);
    }
    profiler.end_frame();
    const std::vector<ZoneSummary> zones = profiler.zones();
    const ZoneSummary* za = find(zones, "self.sibling_a");
    const ZoneSummary* zb = find(zones, "self.sibling_b");
    ASSERT_TRUE(za && zb);
    EXPECT_LT(za->cpu_self.avg, 0.2F);
    EXPECT_FLOAT_EQ(zb->cpu_self.avg, zb->cpu.avg);
}

TEST(ProfileNameCache, ASecondLookupIsAHitWithTheSameId) {
    ProfileNameCache cache(ProfileCategory::pass);
    const auto first = cache.id("cache_test.gbuffer");
    EXPECT_EQ(cache.misses(), 1U);
    for (int frame = 0; frame < 100; ++frame) {
        EXPECT_EQ(cache.id("cache_test.gbuffer"), first);
    }
    EXPECT_EQ(cache.misses(), 1U) << "the name table is not consulted again";
    EXPECT_EQ(cache.size(), 1U);
    EXPECT_EQ(first, engine::core::intern_profile_name("cache_test.gbuffer"));
    EXPECT_EQ(engine::core::profile_category(first), ProfileCategory::pass);

    const auto other = cache.id("cache_test.lighting");
    EXPECT_NE(other, first);
    EXPECT_EQ(cache.misses(), 2U);
    EXPECT_EQ(cache.size(), 2U);
}

} // namespace
