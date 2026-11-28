// Volumetric clouds v2 (R13.4) on the GPU: the quarter-resolution march,
// its temporal accumulation and the composite — clouds appear where the
// coverage puts them, a static camera converges, two runs of the same
// frames are bit-identical (deterministic headless capture), and coverage /
// weather move the result the right way.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/volumetrics_harness.hpp"

namespace {

namespace r = engine::renderer;
using quat::test::HostImage;
using quat::test::VolumetricsHarness;
using quat::test::VolumetricsScene;

[[nodiscard]] VolumetricsScene cumulus_scene() {
    VolumetricsScene s;
    s.clouds.coverage = 0.5F;
    s.clouds.type = 0.85F;
    s.clouds.cirrus_coverage = 0.0F;
    return s;
}

// Mean transmittance of the upper half of the cloud image (the sky).
[[nodiscard]] float mean_transmittance(const HostImage& c) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t y = 0; y < c.extent.height / 2; ++y) {
        for (std::uint32_t x = 0; x < c.extent.width; ++x) {
            sum += c.rgba16f(x, y).a;
            ++n;
        }
    }
    return static_cast<float>(sum / static_cast<double>(n));
}

[[nodiscard]] float mean_abs_diff(const HostImage& a, const HostImage& b) {
    double sum = 0.0;
    for (std::uint32_t y = 0; y < a.extent.height; ++y) {
        for (std::uint32_t x = 0; x < a.extent.width; ++x) {
            const glm::vec4 d = glm::abs(a.rgba16f(x, y) - b.rgba16f(x, y));
            sum += d.r + d.g + d.b + d.a;
        }
    }
    return static_cast<float>(sum / (4.0 * a.extent.width * a.extent.height));
}

// A visual check of the clouds over a plain sky gradient (QUAT_TEST_DUMP_DIR).
void dump_clouds(const HostImage& c, const std::string& name) {
    HostImage img = c;
    for (std::uint32_t y = 0; y < c.extent.height; ++y) {
        for (std::uint32_t x = 0; x < c.extent.width; ++x) {
            const glm::vec4 v = c.rgba16f(x, y);
            const float t = 1.0F - static_cast<float>(y) / static_cast<float>(c.extent.height);
            const glm::vec3 sky = glm::mix(glm::vec3(0.55F, 0.7F, 0.9F), glm::vec3(0.15F, 0.35F, 0.8F), t) * 0.08F;
            const glm::vec3 col = (sky * v.a + glm::vec3(v)) * 8.0F;
            std::uint16_t h[4] = {glm::packHalf1x16(col.r), glm::packHalf1x16(col.g),
                                  glm::packHalf1x16(col.b), glm::packHalf1x16(1.0F)};
            std::memcpy(img.bytes.data() + (static_cast<std::size_t>(y) * c.extent.width + x) * 8, h, 8);
        }
    }
    quat::test::dump_ppm(img, name, 1.0F);
}

TEST(GpuVolumetricClouds, CloudsAppearAndConverge) {
    QUAT_REQUIRE_GPU(g);
    auto h = VolumetricsHarness::create(*g);
    ASSERT_OK(h);
    const VolumetricsScene scene = cumulus_scene();
    std::vector<HostImage> frames;
    for (std::uint64_t f = 0; f < 24; ++f) {
        auto out = (*h)->render(scene, f, 0.0F);
        ASSERT_OK(out);
        frames.push_back(out->clouds);
        if (f == 23) quat::test::dump_ppm(out->composite, "clouds_composite", 1.0F);
    }
    dump_clouds(frames.front(), "clouds_frame0");
    dump_clouds(frames.back(), "clouds_frame23");

    const HostImage& last = frames.back();
    float lo = 1.0F;
    float hi = 0.0F;
    for (std::uint32_t y = 0; y < last.extent.height / 2; ++y) {
        for (std::uint32_t x = 0; x < last.extent.width; ++x) {
            lo = std::min(lo, last.rgba16f(x, y).a);
            hi = std::max(hi, last.rgba16f(x, y).a);
        }
    }
    EXPECT_LT(lo, 0.3F) << "dense clouds somewhere";
    EXPECT_GT(hi, 0.95F) << "and clear sky between them";
    const float t = mean_transmittance(last);
    EXPECT_GT(t, 0.2F);
    EXPECT_LT(t, 0.9F);

    // Temporal stability: with a static camera and clock the accumulated
    // image converges — each frame is closer to the final one than the
    // frames before it, and the last frames barely change.
    const float d0 = mean_abs_diff(frames[0], last);
    const float d8 = mean_abs_diff(frames[8], last);
    const float d16 = mean_abs_diff(frames[16], last);
    EXPECT_LT(d8, d0);
    EXPECT_LT(d16, d8);
    EXPECT_LT(mean_abs_diff(frames[22], last), 2e-3F);
}

TEST(GpuVolumetricClouds, DeterministicForTheSameFrames) {
    QUAT_REQUIRE_GPU(g);
    const VolumetricsScene scene = cumulus_scene();
    std::vector<std::vector<std::uint8_t>> runs;
    for (int run = 0; run < 2; ++run) {
        auto h = VolumetricsHarness::create(*g);
        ASSERT_OK(h);
        HostImage last;
        for (std::uint64_t f = 0; f < 6; ++f) {
            auto out = (*h)->render(scene, f, static_cast<float>(f) / 60.0F); // fixed dt
            ASSERT_OK(out);
            last = out->composite;
        }
        runs.push_back(last.bytes);
    }
    EXPECT_EQ(runs[0], runs[1]) << "headless captures must be reproducible";
}

TEST(GpuVolumetricClouds, CoverageAndOvercastDarkenTheSky) {
    QUAT_REQUIRE_GPU(g);
    auto run = [&](float coverage) -> float {
        auto h = VolumetricsHarness::create(*g);
        EXPECT_TRUE(h.has_value());
        if (!h) return -1.0F;
        VolumetricsScene s = cumulus_scene();
        s.clouds.coverage = coverage;
        HostImage last;
        for (std::uint64_t f = 0; f < 8; ++f) {
            auto out = (*h)->render(s, f, 0.0F);
            EXPECT_TRUE(out.has_value());
            if (!out) return -1.0F;
            last = out->clouds;
        }
        dump_clouds(last, "clouds_coverage_" + std::to_string(static_cast<int>(coverage * 100)));
        return mean_transmittance(last);
    };
    const float clear = run(0.0F);
    const float scattered = run(0.35F);
    const float overcast = run(1.0F);
    EXPECT_NEAR(clear, 1.0F, 1e-3F) << "no coverage, no clouds";
    EXPECT_LT(scattered, clear);
    EXPECT_LT(overcast, 0.5F * scattered);
}

} // namespace
