// Screenshot encoding: sRGB-correct downscaling, PNG, base64.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <stb_image.h>

#include "engine/remote/image_encode.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::remote::base64_encode;
using engine::remote::downscale_srgb;
using engine::remote::encode_screenshot;

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

TEST(ImageEncode, Base64MatchesRfc4648Vectors) {
    EXPECT_EQ(base64_encode(bytes("")), "");
    EXPECT_EQ(base64_encode(bytes("f")), "Zg==");
    EXPECT_EQ(base64_encode(bytes("fo")), "Zm8=");
    EXPECT_EQ(base64_encode(bytes("foo")), "Zm9v");
    EXPECT_EQ(base64_encode(bytes("foob")), "Zm9vYg==");
    EXPECT_EQ(base64_encode(bytes("fooba")), "Zm9vYmE=");
    EXPECT_EQ(base64_encode(bytes("foobar")), "Zm9vYmFy");
    const std::vector<std::uint8_t> high{0xFF, 0xFE, 0x00};
    EXPECT_EQ(base64_encode(high), "//4A");
}

TEST(ImageEncode, SwizzlesBgraWithoutResampling) {
    // 2x1 BGRA with row padding: blue-ish then red-ish.
    const std::vector<std::uint8_t> px{200, 10, 20, 255, 5, 6, 250, 0, 99, 99, 99, 99};
    auto rgb = downscale_srgb(px, 2, 1, 12, /*bgra=*/true, 64);
    ASSERT_OK(rgb);
    EXPECT_EQ(rgb->width, 2U);
    EXPECT_EQ(rgb->height, 1U);
    EXPECT_EQ(rgb->pixels, (std::vector<std::uint8_t>{20, 10, 200, 250, 6, 5}));
}

TEST(ImageEncode, DownscalesInLinearLight) {
    // 400x200 black/white checkerboard -> longest side 100 (a 4x4 box each).
    const std::uint32_t w = 400;
    const std::uint32_t h = 200;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t v = ((x + y) & 1U) != 0 ? 255 : 0;
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    auto rgb = downscale_srgb(px, w, h, w * 4, false, 100);
    ASSERT_OK(rgb);
    EXPECT_EQ(rgb->width, 100U);
    EXPECT_EQ(rgb->height, 50U);
    // 50% linear coverage is sRGB 188 — not the 128 a naive average gives.
    for (const std::uint8_t c : rgb->pixels) EXPECT_NEAR(static_cast<int>(c), 188, 1);

    // Non-integer ratio: every output pixel still averages to the same grey.
    rgb = downscale_srgb(px, w, h, w * 4, false, 77);
    ASSERT_OK(rgb);
    EXPECT_EQ(rgb->width, 77U);
    EXPECT_EQ(rgb->height, 39U); // round(200 * 77 / 400)
    for (const std::uint8_t c : rgb->pixels) EXPECT_NEAR(static_cast<int>(c), 188, 6);
}

TEST(ImageEncode, PngDecodesBack) {
    const std::uint32_t w = 64;
    const std::uint32_t h = 32;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = 10;     // R
        px[i + 1] = 200; // G
        px[i + 2] = 30;  // B
        px[i + 3] = 0;   // alpha is ignored
    }
    auto img = encode_screenshot(px, w, h, w * 4, false, 1024);
    ASSERT_OK(img);
    EXPECT_EQ(img->width, w);
    EXPECT_EQ(img->height, h);
    ASSERT_GT(img->png.size(), 8U);
    EXPECT_EQ(img->png[1], 'P');
    EXPECT_EQ(img->png[2], 'N');
    EXPECT_EQ(img->png[3], 'G');

    int dw = 0;
    int dh = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(img->png.data(), static_cast<int>(img->png.size()),
                                             &dw, &dh, &channels, 3);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(dw, 64);
    EXPECT_EQ(dh, 32);
    EXPECT_EQ(channels, 3);
    EXPECT_EQ(decoded[0], 10);
    EXPECT_EQ(decoded[1], 200);
    EXPECT_EQ(decoded[2], 30);
    stbi_image_free(decoded);
}

TEST(ImageEncode, RejectsBadInput) {
    const std::vector<std::uint8_t> px(16);
    EXPECT_ERR(downscale_srgb(px, 0, 1, 4, false, 64));
    EXPECT_ERR(downscale_srgb(px, 4, 4, 16, false, 64)); // buffer too small
    EXPECT_ERR(downscale_srgb(px, 2, 1, 4, false, 64));  // pitch < width * 4
}

} // namespace
