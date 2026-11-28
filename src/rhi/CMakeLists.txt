# ---------------------------------------------------------------------------
# engine_rhi — thin Vulkan abstraction (instance/device, swapchain, ...).
# ---------------------------------------------------------------------------
add_library(engine_rhi STATIC
    device.cpp
    driver_info.cpp
    queue_selection.cpp
    swapchain.cpp
    gpu_allocator.cpp
    image_upload.cpp
    vma_impl.cpp
    shader_reflection.cpp
    pipeline_cache.cpp
    descriptor_buffer.cpp
    render_graph.cpp
    render_graph_copy.cpp
    gpu_profiler.cpp
    graphics_pipeline.cpp
    compute_pipeline.cpp
    shader_compiler.cpp
    shader_diagnostics.cpp
    shader_hot_reload.cpp
    material_shader_registry.cpp
    material_shader_source.cpp)
target_link_libraries(engine_rhi PUBLIC
    engine_flags engine_core Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
# engine_core is public: shader hot reload's headers use its watcher/process
# types, and the debug messenger routes validation messages into its log ring.
# The hot-reload compile worker runs on a std::thread; hot reload defaults on
# in editor builds (engine_editor_build).
target_link_libraries(engine_rhi PRIVATE Threads::Threads engine_editor_build)
# nlohmann_json is an implementation detail of the reflection parser (PRIVATE);
# glm because the parser reads slangc's JSON through core::JsonFields.
target_link_libraries(engine_rhi PRIVATE nlohmann_json::nlohmann_json glm::glm)
target_include_directories(engine_rhi PUBLIC ${ENGINE_PUBLIC_INCLUDE})

# VMA's implementation is not warning-clean under our strict flags; compile just
# that TU permissively (our code using VMA stays strict).
set_source_files_properties(vma_impl.cpp PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
