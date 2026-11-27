// Guarded Radiance .hdr decoding (R21): a round trip through stb_image_write
// (run-length encoded and flat scanlines), header checks against the texture
// limits and the load budget before anything is allocated, and a corruption
// fuzz run that must only ever produce an image or an error.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <stb_image_write.h>

#include "engine/asset/hdr_decode.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace asset = engine::asset;

// A `w` x `h` gradient with a bright "sun" texel, encoded by stb_image_write
// (RLE scanlines for widths 8..32767, flat ones otherwise).
std::vector<float> gradient(int w, int h) {
    std::vector<float> rgb(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float* p = &rgb[static_cast<std::size_t>((y * w + x) * 3)];
            p[0] = 0.05F + static_cast<float>(x) / static_cast<float>(w);
            p[1] = 0.25F + static_cast<float>(y) / static_cast<float>(h);
            p[2] = 0.5F;
        }
    }
    rgb[3] = 5000.0F; // texel (1, 0)
    return rgb;
}

std::vector<std::byte> encode(int w, int h, const std::vector<float>& rgb) {
    std::vector<std::byte> out;
    stbi_write_hdr_to_func(
        [](void* ctx, void* data, int n) {
            auto* o = static_cast<std::vector<std::byte>*>(ctx);
            const auto* p = static_cast<const std::byte*>(data);
            o->insert(o->end(), p, p + n);
        },
        &out, w, h, 3, rgb.data());
    return out;
}

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

// A header-only file: tiny on disk, enormous when decoded.
std::vector<std::byte> header_only(const std::string& format, const std::string& resolution) {
    std::string s = "#?RADIANCE\n";
    if (!format.empty()) s += "FORMAT=" + format + "\n";
    s += "\n" + resolution + "\n";
    return bytes_of(s);
}

void expect_round_trip(int w, int h) {
    const std::vector<float> rgb = gradient(w, h);
    auto image = asset::decode_radiance_hdr(encode(w, h, rgb));
    ASSERT_OK(image);
    ASSERT_EQ(image->width, static_cast<std::uint32_t>(w));
    ASSERT_EQ(image->height, static_cast<std::uint32_t>(h));
    const std::span<const float> got = image->rgb();
    ASSERT_EQ(got.size(), rgb.size());
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        // RGBE keeps 8 mantissa bits of the largest channel: ~1% relative,
        // and the small channels of a texel share its exponent.
        const std::size_t texel = i / 3 * 3;
        const float scale = std::max({rgb[texel], rgb[texel + 1], rgb[texel + 2]});
        ASSERT_NEAR(got[i], rgb[i], scale * 0.01F + 1e-6F) << "float " << i;
    }
}

} // namespace

TEST(HdrDecode, RoundTripsRunLengthEncodedScanlines) { expect_round_trip(64, 32); }

TEST(HdrDecode, RoundTripsFlatScanlines) { expect_round_trip(5, 3); }

TEST(HdrDecode, RecognisesTheSignature) {
    EXPECT_TRUE(asset::is_radiance_hdr(bytes_of("#?RADIANCE\n")));
    EXPECT_TRUE(asset::is_radiance_hdr(bytes_of("#?RGBE\nx")));
    EXPECT_FALSE(asset::is_radiance_hdr(bytes_of("#?RADIANC")));
    auto png = asset::decode_radiance_hdr(bytes_of("\x89PNG\r\n\x1a\n...."));
    ASSERT_ERR(png);
}

TEST(HdrDecode, RejectsUnsupportedHeaders) {
    const auto rejects = [](const std::vector<std::byte>& f, const char* what) {
        auto r = asset::decode_radiance_hdr(f);
        EXPECT_FALSE(r.has_value()) << what;
    };
    rejects(header_only("32-bit_rle_xyze", "-Y 4 +X 4"), "XYZE pixels");
    rejects(header_only("", "-Y 4 +X 4"), "no FORMAT line");
    rejects(header_only("32-bit_rle_rgbe", "+Y 4 +X 4"), "flipped orientation");
    rejects(header_only("32-bit_rle_rgbe", "-Y 4 -X 4"), "mirrored orientation");
    rejects(header_only("32-bit_rle_rgbe", "-Y 0 +X 4"), "zero height");
    rejects(header_only("32-bit_rle_rgbe", "-Y 99999999999 +X 4"), "overflowing height");
    rejects(header_only("32-bit_rle_rgbe", "-Y 4 +X 4"), "no pixel data");
    rejects(bytes_of("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n"), "no blank line");
    rejects(bytes_of(std::string("#?RADIANCE\n") + std::string(70000, 'a')), "endless header");
}

// Limits apply from the header alone: a 100000 x 100000 claim is refused
// before stb_image allocates the 120 GB it would decode to.
TEST(HdrDecode, RejectsImagesAboveTheLimitsBeforeDecoding) {
    auto huge = asset::decode_radiance_hdr(header_only("32-bit_rle_rgbe", "-Y 100000 +X 100000"));
    ASSERT_ERR(huge);
    EXPECT_NE(huge.error().message.find("limit"), std::string::npos) << huge.error().message;

    const std::vector<std::byte> good = encode(64, 32, gradient(64, 32));
    asset::TextureLimits limits;
    limits.max_dimension = 32;
    EXPECT_ERR(asset::decode_radiance_hdr(good, limits));
    limits = {};
    limits.max_decoded_bytes = 64 * 32 * 12 - 1;
    EXPECT_ERR(asset::decode_radiance_hdr(good, limits));
    limits = {};
    limits.max_file_bytes = 100;
    EXPECT_ERR(asset::decode_radiance_hdr(good, limits));

    asset::DecodeBudget budget(1000);
    auto over = asset::decode_radiance_hdr(good, {}, &budget);
    ASSERT_ERR(over);
    EXPECT_NE(over.error().message.find("budget"), std::string::npos) << over.error().message;
    EXPECT_EQ(budget.used(), 0U);
}

TEST(HdrDecode, LoadsFromAFileAndNamesItInErrors) {
    quat::test::TempDir dir("hdr_decode");
    const std::filesystem::path good = dir.path() / "sky.hdr";
    {
        const std::vector<std::byte> f = encode(16, 8, gradient(16, 8));
        std::ofstream(good, std::ios::binary)
            .write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    }
    auto image = asset::load_radiance_hdr(good);
    ASSERT_OK(image);
    EXPECT_EQ(image->width, 16U);

    auto missing = asset::load_radiance_hdr(dir.path() / "missing.hdr");
    ASSERT_ERR(missing);
    asset::TextureLimits small;
    small.max_file_bytes = 10;
    auto big = asset::load_radiance_hdr(good, small);
    ASSERT_ERR(big);
    EXPECT_NE(big.error().message.find("sky.hdr"), std::string::npos) << big.error().message;
}

// Random corruption of valid files (RLE and flat scanlines, biased into the
// header) must only ever produce an image or an error. Run under valgrind or
// an ASan build for silent out-of-bounds reads; QUAT_HDR_FUZZ_ROUNDS=<n>
// lengthens the campaign.
TEST(HdrDecode, CorruptedFilesNeverCrash) {
    const char* env = std::getenv("QUAT_HDR_FUZZ_ROUNDS");
    const int rounds = env != nullptr ? std::max(1, std::atoi(env)) : 400;
    for (const auto [w, h] : {std::pair{64, 16}, std::pair{6, 5}}) {
        const std::vector<std::byte> good = encode(w, h, gradient(w, h));
        ASSERT_FALSE(good.empty());
        std::mt19937 rng(4321);
        std::uniform_int_distribution<std::size_t> pos(0, good.size() - 1);
        std::uniform_int_distribution<int> byte(0, 255);
        for (int round = 0; round < rounds; ++round) {
            std::vector<std::byte> f = good;
            const int flips = 1 + round % 8;
            for (int i = 0; i < flips; ++i) {
                const std::size_t at = round % 2 == 0 ? pos(rng) % std::min<std::size_t>(80, f.size())
                                                      : pos(rng);
                f[at] = static_cast<std::byte>(byte(rng));
            }
            if (round % 5 == 4) f.resize(pos(rng)); // truncated files
            auto r = asset::decode_radiance_hdr(f);
            if (r) {
                EXPECT_GT(r->width, 0U);
                EXPECT_EQ(r->rgb().size(), static_cast<std::size_t>(r->width) * r->height * 3);
            }
        }
    }
}
