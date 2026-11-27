// Temporal anti-aliasing across frames (audit S1 A1/A2/A14).
//
// Frames are driven the way the engine's frame loop drives them: two
// frame-in-flight slots, each with its own TaaPass, sharing one TaaHistory and
// alternating every frame. The current frame is a checkerboard whose phase
// flips every frame (what sub-pixel jitter does to detail finer than a pixel):
// a working history accumulates it to its mean, a broken one shows the raw
// alternation.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/taa_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;
using quat::test::image_barrier;
using quat::test::submit_blocking;

constexpr std::uint8_t k_hi = 204; // 0.8
constexpr std::uint8_t k_lo = 51;  // 0.2

[[nodiscard]] std::unexpected<Error> fail(std::string what) {
    return std::unexpected(Error{std::move(what)});
}

// RGBA8 checkerboard: grey k_hi where (x + y + phase) is even, k_lo elsewhere.
std::vector<std::uint8_t> checker(VkExtent2D e, std::uint32_t phase) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(e.width) * e.height * 4);
    for (std::uint32_t y = 0; y < e.height; ++y) {
        for (std::uint32_t x = 0; x < e.width; ++x) {
            const std::uint8_t v = ((x + y + phase) % 2 == 0) ? k_hi : k_lo;
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * e.width + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    return px;
}

// Offscreen inputs (current colour, depth) and output target for one extent,
// plus the glue to run one TAA frame through a render graph and read back the
// target bytes.
class Rig {
public:
    [[nodiscard]] static std::expected<Rig, Error> create(quat::test::GpuContext& g, VkFormat fmt,
                                                          VkExtent2D extent) {
        Rig rig;
        rig.g_ = &g;
        rig.fmt_ = fmt;
        rig.extent_ = extent;
        const VkDevice dev = g.device.handle();
        auto cur = g.allocator.create_image(fmt, extent, VK_IMAGE_USAGE_SAMPLED_BIT |
                                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto depth = g.allocator.create_image(
            VK_FORMAT_D32_SFLOAT, extent,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        auto target = g.allocator.create_image(fmt, extent, VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!cur || !depth || !target) return fail("taa test: image creation failed");
        auto cur_view = rhi::create_image_view(dev, cur->handle(), fmt);
        auto depth_view = rhi::create_image_view(dev, depth->handle(), VK_FORMAT_D32_SFLOAT,
                                                 VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!cur_view || !depth_view) return fail("taa test: view creation failed");
        const VkDeviceSize bytes = rig.byte_size();
        auto staging = g.allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        auto readback = g.allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!staging || !readback) return fail("taa test: buffer creation failed");
        rig.cur_ = std::move(*cur);
        rig.cur_view_ = std::move(*cur_view);
        rig.depth_ = std::move(*depth);
        rig.depth_view_ = std::move(*depth_view);
        rig.target_ = std::move(*target);
        rig.staging_ = std::move(*staging);
        rig.readback_ = std::move(*readback);
        rig.pool_ = std::make_unique<rhi::TransientImagePool>(g.device, g.allocator);

        // Constant depth: any value reconstructs a valid world point.
        const VkImage depth_img = rig.depth_.handle();
        const bool ok = submit_blocking(g.device, [&](VkCommandBuffer cmd) {
            image_barrier(cmd, depth_img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkClearDepthStencilValue dc{0.5F, 0};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd, depth_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dc,
                                        1, &range);
            image_barrier(cmd, depth_img, VK_IMAGE_ASPECT_DEPTH_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!ok) return fail("taa test: depth clear failed");
        return rig;
    }

    // Upload `pixels` (byte_size() bytes, in the image's own channel order) as
    // the current frame, resolve it with `taa` and return the target's bytes.
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, Error>
    frame(r::TaaPass& taa, const std::vector<std::uint8_t>& pixels, const glm::mat4& view_proj) {
        if (pixels.size() != byte_size()) return fail("taa test: wrong pixel count");
        std::memcpy(staging_.mapped(), pixels.data(), pixels.size());

        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rhi::RenderGraph graph(*pool_);
        const rhi::ResourceHandle h_cur = graph.import_image(
            "test_current", cur_.handle(), cur_view_.handle(), fmt_, extent_, ro, ro);
        const rhi::ResourceHandle h_depth =
            graph.import_image("test_depth", depth_.handle(), depth_view_.handle(),
                               VK_FORMAT_D32_SFLOAT, extent_, ro, ro);
        // No final layout: the copy-out below transitions the target itself.
        const rhi::ResourceHandle h_target =
            graph.import_image("test_target", target_.handle(), VK_NULL_HANDLE, fmt_, extent_,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED);
        if (auto a = taa.add_to_graph(graph, h_cur, h_depth, h_target, extent_, view_proj); !a) {
            return std::unexpected(a.error());
        }
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());

        const VkImage cur_img = cur_.handle();
        const VkImage target_img = target_.handle();
        const VkBuffer staging = staging_.handle();
        const VkBuffer readback = readback_.handle();
        const VkExtent2D e = extent_;
        const bool ok = submit_blocking(g_->device, [&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {e.width, e.height, 1};
            image_barrier(cmd, cur_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(cmd, staging, cur_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
            image_barrier(cmd, cur_img, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ro, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

            graph.execute(cmd);

            image_barrier(cmd, target_img, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdCopyImageToBuffer(cmd, target_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback,
                                   1, &region);
            VkMemoryBarrier2 host{};
            host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &host;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        if (!ok) return fail("taa test: frame submit failed");
        pool_->reset(); // the frame finished: transient images are free again

        const auto* px = static_cast<const std::uint8_t*>(readback_.mapped());
        return std::vector<std::uint8_t>(px, px + byte_size());
    }

    [[nodiscard]] std::size_t byte_size() const {
        return static_cast<std::size_t>(extent_.width) * extent_.height * 4;
    }
    [[nodiscard]] std::size_t centre() const {
        return (static_cast<std::size_t>(extent_.height / 2) * extent_.width + extent_.width / 2) *
               4;
    }

private:
    quat::test::GpuContext* g_ = nullptr;
    VkFormat                fmt_ = VK_FORMAT_UNDEFINED;
    VkExtent2D              extent_{};
    rhi::GpuImage           cur_;
    rhi::ImageView          cur_view_;
    rhi::GpuImage           depth_;
    rhi::ImageView          depth_view_;
    rhi::GpuImage           target_;
    rhi::GpuBuffer          staging_;
    rhi::GpuBuffer          readback_;
    std::unique_ptr<rhi::TransientImagePool> pool_;
};

// Largest per-byte difference between two images (alpha included).
int max_abs_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        worst = std::max(worst, std::abs(int{a[i]} - int{b[i]}));
    }
    return worst;
}

// Two frame-in-flight slots sharing one history, as the engine sets them up.
struct Slots {
    std::shared_ptr<r::TaaHistory> history;
    r::TaaPass                     pass[2];
};

std::expected<Slots, Error> make_slots(quat::test::GpuContext& g, VkFormat fmt) {
    Slots s;
    s.history = std::make_shared<r::TaaHistory>(g.device, g.allocator, 2U);
    for (r::TaaPass& p : s.pass) {
        auto created = r::TaaPass::create(g.device, g.allocator, g.cache, g.shader_dir, fmt,
                                          s.history);
        if (!created) return std::unexpected(created.error());
        p = std::move(*created);
    }
    return s;
}

// A perspective camera like the engine's (Vulkan Y flip, depth [0,1]).
glm::mat4 camera_view_proj() {
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    return proj * glm::lookAt(glm::vec3(2.0F, 1.5F, 3.0F), glm::vec3(0.0F),
                              glm::vec3(0.0F, 1.0F, 0.0F));
}

} // namespace

// A2: each frame must read the history the previous frame wrote, even though
// consecutive frames are recorded by different slots. With per-slot history
// every slot read a never-written image and the output was the raw input.
TEST(GpuTaa, HistoryAccumulatesAcrossFrameSlots) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkExtent2D extent{16, 16};
    auto rig = Rig::create(*g, fmt, extent);
    ASSERT_OK(rig);
    auto slots = make_slots(*g, fmt);
    ASSERT_OK(slots);
    const glm::mat4 vp = camera_view_proj();
    const std::size_t c = rig->centre();

    constexpr int k_frames = 48;
    std::vector<std::vector<std::uint8_t>> out;
    for (int i = 0; i < k_frames; ++i) {
        const std::vector<std::uint8_t> input = checker(extent, static_cast<std::uint32_t>(i));
        auto o = rig->frame(slots->pass[i % 2], input, vp);
        ASSERT_OK(o) << "frame " << i;
        if (i == 0) {
            EXPECT_LE(max_abs_diff(*o, input), 1) << "the first frame has no history";
        }
        out.push_back(std::move(*o));
    }
    EXPECT_EQ(slots->history->frame_count(), static_cast<std::uint64_t>(k_frames));
    EXPECT_TRUE(slots->history->valid());

    // Frame 1 (slot B) blends with frame 0's output (slot A): 0.1 * 0.2 +
    // 0.9 * 0.8 = 0.74 at the centre. A broken history shows the raw 0.2.
    EXPECT_NEAR(out[1][c], 189, 3) << "frame 1 did not read frame 0's history";

    // Converged: the alternating input (0.2 <-> 0.8) accumulates to its mean;
    // the steady state oscillates only 0.484 <-> 0.516.
    const int last = out[k_frames - 1][c];
    const int prev = out[k_frames - 2][c];
    EXPECT_NEAR(last, 128, 8);
    EXPECT_NEAR(prev, 128, 8);
    EXPECT_LE(std::abs(last - prev), 12) << "history did not converge (input swings 153)";
}

// A14: reprojection between unjittered matrices maps a static camera's pixels
// exactly onto themselves, so a static image stays pixel-exact (no resampling
// blur). The control run shows the test is sensitive: reprojecting between
// view-projections half a pixel apart (what jittered matrices did) blurs it.
TEST(GpuTaa, StaticCameraReprojectsOntoItself) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkExtent2D extent{16, 16};
    auto rig = Rig::create(*g, fmt, extent);
    ASSERT_OK(rig);
    const std::vector<std::uint8_t> image = checker(extent, 0);
    const glm::mat4 vp = camera_view_proj();

    auto slots = make_slots(*g, fmt);
    ASSERT_OK(slots);
    std::vector<std::uint8_t> last;
    for (int i = 0; i < 8; ++i) {
        auto o = rig->frame(slots->pass[i % 2], image, vp);
        ASSERT_OK(o) << "frame " << i;
        last = std::move(*o);
    }
    EXPECT_LE(max_abs_diff(last, image), 1) << "static camera: history was resampled";

    // Control: alternate the view-projection by half a pixel (jittered
    // matrices) — the history is then sampled between texels and blurs.
    auto control = make_slots(*g, fmt);
    ASSERT_OK(control);
    for (int i = 0; i < 8; ++i) {
        glm::mat4 jitter(1.0F);
        jitter[3][0] = (i % 2 == 0 ? 0.5F : -0.5F) / static_cast<float>(extent.width);
        auto o = rig->frame(control->pass[i % 2], image, jitter * vp);
        ASSERT_OK(o) << "control frame " << i;
        last = std::move(*o);
    }
    EXPECT_GT(max_abs_diff(last, image), 40) << "control run unexpectedly stayed sharp";
}

// A2: a resize (new extent) recreates the history and drops its validity, so
// the first frame at the new size is exactly the current frame; accumulation
// resumes on the next one. invalidate() does the same without a resize.
TEST(GpuTaa, ResizeResetsHistory) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const glm::mat4 vp = camera_view_proj();
    auto slots = make_slots(*g, fmt);
    ASSERT_OK(slots);

    constexpr VkExtent2D small{16, 16};
    auto rig_small = Rig::create(*g, fmt, small);
    ASSERT_OK(rig_small);
    int frame = 0;
    for (; frame < 6; ++frame) {
        auto o = rig_small->frame(slots->pass[frame % 2],
                                  checker(small, static_cast<std::uint32_t>(frame)), vp);
        ASSERT_OK(o) << "frame " << frame;
    }
    ASSERT_TRUE(slots->history->valid());

    constexpr VkExtent2D big{24, 20};
    auto rig_big = Rig::create(*g, fmt, big);
    ASSERT_OK(rig_big);
    const std::vector<std::uint8_t> first = checker(big, 0);
    auto o = rig_big->frame(slots->pass[frame % 2], first, vp);
    ASSERT_OK(o);
    ++frame;
    EXPECT_EQ(slots->history->extent().width, big.width);
    EXPECT_EQ(slots->history->extent().height, big.height);
    EXPECT_LE(max_abs_diff(*o, first), 1) << "stale history blended after a resize";

    // History is live again at the new size: the flipped checker blends.
    const std::size_t c = rig_big->centre();
    auto next = rig_big->frame(slots->pass[frame % 2], checker(big, 1), vp);
    ASSERT_OK(next);
    ++frame;
    EXPECT_NEAR((*next)[c], 189, 3) << "history not accumulating after the resize";

    // Explicit invalidation (camera cut) behaves like a resize.
    slots->history->invalidate();
    const std::vector<std::uint8_t> cut = checker(big, 0);
    auto after_cut = rig_big->frame(slots->pass[frame % 2], cut, vp);
    ASSERT_OK(after_cut);
    EXPECT_LE(max_abs_diff(*after_cut, cut), 1) << "history blended after invalidate()";
}

// The resolve writes RGBA8 (always storage-capable, A10) and byte-copies into
// the target; for a BGRA target the channels must still land in place.
TEST(GpuTaa, BgraTargetKeepsChannelOrder) {
    QUAT_REQUIRE_GPU(g);
    constexpr VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    constexpr VkExtent2D extent{8, 8};
    auto rig = Rig::create(*g, fmt, extent);
    ASSERT_OK(rig);
    auto slots = make_slots(*g, fmt);
    ASSERT_OK(slots);

    // Memory order B, G, R, A: a distinctly orange pixel.
    std::vector<std::uint8_t> input(rig->byte_size());
    for (std::size_t i = 0; i < input.size(); i += 4) {
        input[i + 0] = 40;
        input[i + 1] = 120;
        input[i + 2] = 220;
        input[i + 3] = 255;
    }
    for (int i = 0; i < 3; ++i) {
        auto o = rig->frame(slots->pass[i % 2], input, camera_view_proj());
        ASSERT_OK(o) << "frame " << i;
        EXPECT_LE(max_abs_diff(*o, input), 1) << "frame " << i << ": channels swapped";
    }
}
