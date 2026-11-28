# ---------------------------------------------------------------------------
# engine_renderer — high-level passes built on the RHI. Starts with the deferred
# GBuffer pass (mesh_pass); lighting/tonemap/imgui passes follow in later slices.
# ---------------------------------------------------------------------------
add_library(engine_renderer STATIC
    csm.cpp
    draw_batching.cpp
    render_settings.cpp
    grass.cpp
    grass_pass.cpp
    gtao_pass.cpp
    hiz_pass.cpp
    instance_buffer.cpp
    non_finite_counters.cpp
    mesh_pass.cpp
    material_pipelines.cpp
    lighting_pass.cpp
    light_clusters.cpp
    light_cull_pass.cpp
    particles.cpp
    particle_emitters.cpp
    particle_pass.cpp
    particle_sort.cpp
    particle_draw.cpp
    tonemap_pass.cpp
    color_grading.cpp
    color_lut.cpp
    physical_camera.cpp
    post_stage.cpp
    dof_pass.cpp
    motion_blur_pass.cpp
    skinning_pass.cpp
    shadow_casters.cpp
    shadow_pass.cpp
    transparent_pass.cpp
    ibl_pass.cpp
    dynamic_ibl.cpp
    compute_kernel.cpp
    hdri_environment.cpp
    hdri_sun.cpp
    ssr_pass.cpp
    bloom_pass.cpp
    exposure_pass.cpp
    taa_pass.cpp
    jitter.cpp
    velocity.cpp
    velocity_pass.cpp
    motion_history.cpp
    upscaler.cpp
    atmosphere_pass.cpp
    terrain_pass.cpp
    terrain_gpu_tiles.cpp
    terrain_synth_pass.cpp
    terrain_shadow.cpp
    wind_field_pass.cpp
    cloud_shadow_pass.cpp
    cloud_shadow_reference.cpp)
target_link_libraries(engine_renderer PUBLIC
    engine_flags engine_rhi engine_asset engine_terrain engine_weather glm::glm)
target_include_directories(engine_renderer PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# The ImGui overlay pass is editor-only. It is a from-scratch renderer on the
# engine RHI (descriptor buffers — the stock imgui_impl_vulkan backend uses
# descriptor pools, which may not be mixed with descriptor buffers in one
# command buffer). imgui_pass.cpp is the only renderer TU allowed to include
# ImGui headers.
if(ENGINE_EDITOR)
    target_sources(engine_renderer PRIVATE imgui_pass.cpp)
    target_link_libraries(engine_renderer PRIVATE imgui::imgui)
endif()
