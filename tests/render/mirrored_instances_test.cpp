// Mirrored instances in passes that cull: partition_mirrored splits each
// draw's instance range into unmirrored then mirrored instances (stable,
// motion kept parallel), and the transparent pass culls front faces for the
// mirrored range of a single-sided material (PR #91 review: scatter.mirror on
// a single-sided BLEND mesh rendered half its instances inside-out).

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/transparent_pass.hpp"

namespace {

namespace r = engine::renderer;

// Instance `id` at x = id, mirrored along X when `mirrored`.
r::InstanceData instance(int id, bool mirrored) {
    const glm::mat4 m = glm::scale(glm::translate(glm::mat4(1.0F), glm::vec3(float(id), 0, 0)),
                                   glm::vec3(mirrored ? -1.0F : 1.0F, 1.0F, 1.0F));
    return r::make_instance_data(m);
}

int id_of(const r::InstanceData& d) { return static_cast<int>(d.model[3].x); }

} // namespace

TEST(MirroredInstances, InstanceDataCarriesTheHandedness) {
    EXPECT_FALSE(r::is_mirrored(instance(0, false)));
    EXPECT_TRUE(r::is_mirrored(instance(0, true)));
}

TEST(MirroredInstances, PartitionSplitsEachDrawStablyAndKeepsMotionParallel) {
    r::InstancedDrawList list;
    // Draw 0: instances 0..4 (mirrored: 1, 3); draw 1: 5..7 (all mirrored);
    // draw 2: 8..9 (none mirrored).
    const bool mirrored[] = {false, true, false, true, false, true, true, true, false, false};
    for (int i = 0; i < 10; ++i) {
        list.instances.push_back(instance(i, mirrored[i]));
        r::InstanceMotion m{};
        m.prev_model = list.instances.back().model;
        list.motion.push_back(m);
    }
    list.draws = {{.first_instance = 0, .instance_count = 5},
                  {.first_instance = 5, .instance_count = 3},
                  {.first_instance = 8, .instance_count = 2}};

    const std::vector<std::uint32_t> unmirrored = r::partition_mirrored(list);
    ASSERT_EQ(unmirrored, (std::vector<std::uint32_t>{3, 0, 2}));
    std::vector<int> order;
    for (const r::InstanceData& d : list.instances) order.push_back(id_of(d));
    EXPECT_EQ(order, (std::vector<int>{0, 2, 4, 1, 3, 5, 6, 7, 8, 9}));
    for (std::size_t i = 0; i < list.instances.size(); ++i) {
        EXPECT_EQ(list.motion[i].prev_model, list.instances[i].model) << i;
        EXPECT_EQ(r::is_mirrored(list.instances[i]), mirrored[id_of(list.instances[i])]);
    }
}

TEST(MirroredInstances, TransparentCullModeFlipsForMirroredSingleSidedDraws) {
    EXPECT_EQ(r::transparent_cull_mode(false, false), VK_CULL_MODE_BACK_BIT);
    EXPECT_EQ(r::transparent_cull_mode(false, true), VK_CULL_MODE_FRONT_BIT);
    EXPECT_EQ(r::transparent_cull_mode(true, false), VK_CULL_MODE_NONE);
    EXPECT_EQ(r::transparent_cull_mode(true, true), VK_CULL_MODE_NONE);
}
