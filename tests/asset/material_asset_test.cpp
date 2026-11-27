// Material upload: the parameter buffer is created and addressable, the CPU
// copy of the parameters is preserved, and unset texture slots stay null.

#include <gtest/gtest.h>

#include "engine/asset/material_asset.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/glm_near.hpp"

namespace {

using namespace engine::asset;

TEST(GpuMaterialAsset, UploadPreservesParamsAndLeavesUnsetTexturesNull) {
    QUAT_REQUIRE_GPU(g);

    PbrMaterialParams params;
    params.base_color_factor = {0.8F, 0.4F, 0.2F, 1.0F};
    params.emissive_factor = {0.1F, 0.0F, 0.0F, 0.0F};
    params.metallic_factor = 0.25F;
    params.roughness_factor = 0.6F;
    params.alpha_cutoff = 0.33F;
    params.flags = material_has_base_color | material_has_metallic_roughness;

    auto material = upload_material(params, MaterialTextures{}, g->allocator, g->transfer);
    ASSERT_OK(material);
    EXPECT_TRUE(material->valid()) << "no param buffer or device address";
    EXPECT_EQ(material->param_buffer.size(), sizeof(PbrMaterialParams));

    EXPECT_EQ(material->params.flags, material_has_base_color | material_has_metallic_roughness);
    EXPECT_VEC_NEAR(material->params.base_color_factor, params.base_color_factor, 0.0F);
    EXPECT_VEC_NEAR(material->params.emissive_factor, params.emissive_factor, 0.0F);
    EXPECT_EQ(material->params.metallic_factor, params.metallic_factor);
    EXPECT_EQ(material->params.roughness_factor, params.roughness_factor);
    EXPECT_EQ(material->params.alpha_cutoff, params.alpha_cutoff);

    const MaterialTextures& t = material->textures;
    EXPECT_FALSE(t.base_color.valid());
    EXPECT_FALSE(t.normal.valid());
    EXPECT_FALSE(t.metallic_roughness.valid());
    EXPECT_FALSE(t.emissive.valid());
    EXPECT_FALSE(t.occlusion.valid());
    EXPECT_FALSE(t.thickness.valid());
}

} // namespace
