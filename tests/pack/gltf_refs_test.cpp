// External files of glTF models (.gltf JSON and the GLB JSON chunk).

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/pack/gltf_refs.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

using engine::pack::gltf_document_uris;
using engine::pack::gltf_file_uris;
using nlohmann::json;

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::string glb(const std::string& json_text) {
    std::string chunk = json_text;
    while (chunk.size() % 4 != 0) chunk.push_back(' ');
    std::string out;
    put_u32(out, 0x46546C67);
    put_u32(out, 2);
    put_u32(out, static_cast<std::uint32_t>(12 + 8 + chunk.size()));
    put_u32(out, static_cast<std::uint32_t>(chunk.size()));
    put_u32(out, 0x4E4F534A);
    return out + chunk;
}

TEST(GltfRefs, ExternalUris) {
    const json doc = json::parse(R"({
        "buffers": [{"uri": "mesh.bin", "byteLength": 4}, {"byteLength": 8},
                    {"uri": "data:application/octet-stream;base64,AAAA"}],
        "images": [{"uri": "textures/base%20color.png"}, {"bufferView": 0}]
    })");
    auto uris = gltf_document_uris(doc);
    ASSERT_OK(uris);
    EXPECT_EQ(*uris, (std::vector<std::string>{"mesh.bin", "textures/base color.png"}));
}

TEST(GltfRefs, RejectsRemoteAndMalformedUris) {
    EXPECT_ERR(gltf_document_uris(json::parse(R"({"images": [{"uri": "https://x/y.png"}]})")));
    EXPECT_ERR(gltf_document_uris(json::parse(R"({"images": [{"uri": "file:///y.png"}]})")));
    EXPECT_ERR(gltf_document_uris(json::parse(R"({"images": [{"uri": 5}]})")));
    EXPECT_ERR(gltf_document_uris(json::parse(R"({"buffers": {}})")));
    EXPECT_ERR(gltf_document_uris(json::parse(R"({"images": [{"uri": "a%2"}]})")));
    EXPECT_ERR(gltf_document_uris(json::parse(R"({"images": [{"uri": "a%00b"}]})")));
    EXPECT_OK(gltf_document_uris(json::object()));
}

TEST(GltfRefs, PercentDecoding) {
    EXPECT_EQ(*engine::pack::percent_decode("a%20b%2Fc"), "a b/c");
    EXPECT_ERR(engine::pack::percent_decode("%zz"));
    EXPECT_ERR(engine::pack::percent_decode("%"));
}

TEST(GltfRefs, ReadsGltfAndGlbFiles) {
    quat::test::TempProject dir("quat_gltf_refs");
    dir.write("a.gltf", R"({"asset": {"version": "2.0"}, "buffers": [{"uri": "a.bin"}]})");
    auto a = gltf_file_uris(dir.path() / "a.gltf");
    ASSERT_OK(a);
    EXPECT_EQ(*a, std::vector<std::string>{"a.bin"});

    dir.write("b.glb", glb(R"({"asset": {"version": "2.0"}, "images": [{"uri": "tex.ktx2"}]})"));
    auto b = gltf_file_uris(dir.path() / "b.glb");
    ASSERT_OK(b);
    EXPECT_EQ(*b, std::vector<std::string>{"tex.ktx2"});

    std::string truncated = glb(R"({"asset": {}})");
    truncated.resize(truncated.size() - 4);
    dir.write("c.glb", truncated);
    EXPECT_ERR(gltf_file_uris(dir.path() / "c.glb"));
    dir.write("d.glb", "not a glb at all");
    EXPECT_ERR(gltf_file_uris(dir.path() / "d.glb"));
    dir.write("e.gltf", "{broken");
    EXPECT_ERR(gltf_file_uris(dir.path() / "e.gltf"));
}

} // namespace
