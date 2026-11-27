// Asset ids and the asset manager cache: id stability, cache hits, default
// fallback on failure, lookup by id.

#include <expected>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"

namespace {

using engine::asset::asset_id_from_path;
using engine::asset::AssetId;
using engine::asset::AssetManager;
using engine::asset::AssetState;
using engine::core::Error;

// The empty path hashes zero bytes: canonical XXH64("") with seed 0.
TEST(AssetId, EmptyPathHashesToTheXxHash64ReferenceVector) {
    EXPECT_EQ(asset_id_from_path("").value, 0xEF46DB3751D8E999ULL)
        << "xxHash64 implementation broken";
}

TEST(AssetId, StableAcrossEquivalentPaths) {
    EXPECT_EQ(asset_id_from_path("meshes/duck.gltf").value,
              asset_id_from_path("./meshes/duck.gltf").value);
    EXPECT_EQ(asset_id_from_path("meshes/duck.gltf").value,
              asset_id_from_path("meshes/duck.gltf").value);
}

TEST(AssetId, DistinctPathsDiffer) {
    EXPECT_NE(asset_id_from_path("meshes/duck.gltf").value,
              asset_id_from_path("meshes/box.gltf").value);
}

struct DummyAsset {
    int         value = 0;
    std::string tag = "default";
};

class AssetManagerCache : public ::testing::Test {
protected:
    AssetManager manager;

    void SetUp() override { manager.set_default(DummyAsset{-1, "fallback"}); }

    [[nodiscard]] engine::asset::AssetHandle<DummyAsset> load_duck() {
        return manager.load<DummyAsset>(
            "meshes/duck.gltf", [](const std::filesystem::path&) -> std::expected<DummyAsset, Error> {
                return DummyAsset{42, "duck"};
            });
    }
};

TEST_F(AssetManagerCache, SuccessfulLoadYieldsTheLoadedAsset) {
    const auto duck = load_duck();
    ASSERT_TRUE(duck.valid());
    EXPECT_TRUE(duck.is_loaded());
    EXPECT_EQ(duck->value, 42);
    EXPECT_EQ(duck->tag, "duck");
}

TEST_F(AssetManagerCache, SecondLoadOfAPathIsServedFromTheCache) {
    const auto duck = load_duck();
    bool loader_ran = false;
    const auto again = manager.load<DummyAsset>(
        "meshes/duck.gltf", [&](const std::filesystem::path&) -> std::expected<DummyAsset, Error> {
            loader_ran = true;
            return DummyAsset{0, "should-not-run"};
        });
    EXPECT_FALSE(loader_ran);
    ASSERT_TRUE(again.valid());
    EXPECT_EQ(again.id().value, duck.id().value);
    EXPECT_EQ(again->value, 42);
    EXPECT_EQ(manager.count<DummyAsset>(), 1U);
}

TEST_F(AssetManagerCache, FailedLoadFallsBackToTheDefault) {
    const auto bad = manager.load<DummyAsset>(
        "meshes/missing.gltf",
        [](const std::filesystem::path&) -> std::expected<DummyAsset, Error> {
            return std::unexpected(Error{"simulated load failure"});
        });
    ASSERT_TRUE(bad.valid());
    EXPECT_EQ(bad.state(), AssetState::failed);
    EXPECT_EQ(bad->value, -1);
    EXPECT_EQ(bad->tag, "fallback");
}

// Audit C16: a failed slot used to be cached forever, so a file fixed (or
// created) after the first attempt never loaded until restart.
TEST_F(AssetManagerCache, FailedSlotIsRetriedOnTheNextLoad) {
    int attempts = 0;
    const auto loader = [&](const std::filesystem::path&) -> std::expected<DummyAsset, Error> {
        ++attempts;
        if (attempts == 1) return std::unexpected(Error{"not there yet"});
        return DummyAsset{7, "fixed"};
    };
    const auto first = manager.load<DummyAsset>("meshes/late.gltf", loader);
    ASSERT_EQ(first.state(), AssetState::failed);

    const auto second = manager.load<DummyAsset>("meshes/late.gltf", loader);
    EXPECT_EQ(attempts, 2);
    ASSERT_TRUE(second.is_loaded());
    EXPECT_EQ(second->value, 7);
    // Same slot: the handle from the failed attempt sees the asset too.
    EXPECT_TRUE(first.is_loaded());
    EXPECT_EQ(first->tag, "fixed");
    EXPECT_EQ(manager.count<DummyAsset>(), 1U);

    // Loaded now: further loads are cache hits again.
    (void)manager.load<DummyAsset>("meshes/late.gltf", loader);
    EXPECT_EQ(attempts, 2);
}

TEST_F(AssetManagerCache, RetryThatFailsAgainKeepsTheDefault) {
    const auto failing = [](const std::filesystem::path&) -> std::expected<DummyAsset, Error> {
        return std::unexpected(Error{"still broken"});
    };
    (void)manager.load<DummyAsset>("meshes/broken.gltf", failing);
    const auto again = manager.load<DummyAsset>("meshes/broken.gltf", failing);
    EXPECT_EQ(again.state(), AssetState::failed);
    EXPECT_EQ(again->tag, "fallback");
}

TEST_F(AssetManagerCache, GetFindsCachedAssetsAndNothingForUnknownIds) {
    const auto duck = load_duck();
    const auto found = manager.get<DummyAsset>(duck.id());
    ASSERT_TRUE(found.valid());
    EXPECT_EQ(found->value, 42);
    EXPECT_FALSE(manager.get<DummyAsset>(AssetId{0xDEADBEEF}).valid());
}

} // namespace
