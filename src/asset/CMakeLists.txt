# ---------------------------------------------------------------------------
# engine_asset — asset identity, ref-counted handles, synchronous load path,
# and asset types (MeshAsset). MeshAsset owns GpuBuffers, so engine_rhi and glm
# are PUBLIC: any consumer of the asset headers needs them transitively.
# ---------------------------------------------------------------------------
# engine_image — graphics-free texture data, limits and the guarded PNG/JPEG
# and Radiance HDR decoders (stb_image), shared by engine_asset and the cooker.
add_library(engine_image STATIC
    alpha_coverage.cpp
    texture_data.cpp
    image_decode.cpp
    cube_lut.cpp
    hdr_decode.cpp)
target_link_libraries(engine_image PUBLIC engine_flags)
target_link_libraries(engine_image PRIVATE engine_stb_image)
target_include_directories(engine_image PUBLIC ${ENGINE_PUBLIC_INCLUDE})
target_include_directories(engine_image PRIVATE ${Stb_INCLUDE_DIR})

# libktx (KTX2 parsing, Zstd, Basis Universal transcoding) backs the KTX2
# loader. A program can hold only one libktx archive (ktx and ktx_read share
# objects: duplicate symbols), so engine_asset's link interface picks it for
# the executable being linked: KTX::ktx_read (decoder only; ports/ktx,
# game_has_no_texture_encoder) unless the executable sets QUAT_KTX_FULL, as
# the cooker and the tests do (they need the full KTX::ktx for encoding).
# The bare $<TARGET_PROPERTY:QUAT_KTX_FULL> is evaluated on the consuming
# executable, and as engine_asset's own dependency the archive is emitted
# after every copy of libengine_asset.a on the link line.
add_library(engine_asset STATIC
    asset_manager.cpp
    material_asset.cpp
    ktx2_validate.cpp
    ktx2_transcode.cpp
    texture_upload.cpp)
target_link_libraries(engine_asset PUBLIC engine_flags engine_rhi engine_image glm::glm)
target_link_libraries(engine_asset PRIVATE
    $<IF:$<BOOL:$<TARGET_PROPERTY:QUAT_KTX_FULL>>,KTX::ktx,KTX::ktx_read>)
target_include_directories(engine_asset PUBLIC ${ENGINE_PUBLIC_INCLUDE})
