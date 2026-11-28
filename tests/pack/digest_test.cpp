// SHA-256 for the package manifest (the OpenSSL EVP wrapper): FIPS 180-4
// test vectors, streaming in odd chunk sizes, files.

#include <string>

#include <gtest/gtest.h>

#include "engine/pack/digest.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

using engine::pack::Sha256;

std::string hex(std::string_view data) {
    auto h = engine::pack::sha256_hex(data);
    EXPECT_TRUE(h.has_value()) << (h ? "" : h.error().message);
    return h ? *h : std::string();
}

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hex(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 55 / 56 / 64 bytes: the padding block boundaries.
    EXPECT_EQ(hex(std::string(55, 'a')),
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(hex(std::string(56, 'a')),
              "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_EQ(hex(std::string(64, 'a')),
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(Sha256, StreamingMatchesOneShot) {
    std::string data;
    for (int i = 0; i < 5000; ++i) data.push_back(static_cast<char>(i * 31 + 7));
    const std::string expected = hex(data);
    for (const std::size_t chunk : {1U, 3U, 63U, 64U, 65U, 1000U}) {
        Sha256 h;
        for (std::size_t i = 0; i < data.size(); i += chunk) {
            h.update(std::string_view(data).substr(i, chunk));
        }
        auto got = h.hex();
        ASSERT_OK(got);
        EXPECT_EQ(*got, expected) << "chunk " << chunk;
        EXPECT_ERR(h.hex()) << "a finalised digest is spent";
    }
}

TEST(Sha256, Files) {
    quat::test::TempProject dir("quat_sha256");
    dir.write("a.txt", "abc");
    std::uint64_t bytes = 0;
    auto digest = engine::pack::sha256_file(dir.path() / "a.txt", bytes);
    ASSERT_OK(digest);
    EXPECT_EQ(*digest, hex("abc"));
    EXPECT_EQ(bytes, 3U);
    EXPECT_ERR(engine::pack::sha256_file(dir.path() / "missing", bytes));
}

} // namespace
