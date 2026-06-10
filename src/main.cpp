// quat-3d engine entry point.
//
// Phase 2, Slice 2.5: a hard-coded triangle is drawn via the render graph. Each
// frame builds a RenderGraph that imports the acquired swapchain image and runs
// a single GraphicsPass (dynamic rendering) that clears to black and draws the
// triangle from mesh.slang. The graph inserts the layout barriers and the
// trailing transition to PRESENT_SRC. Two frames in flight.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/renderer/transparent_pass.hpp"

#ifdef ENGINE_EDITOR
#include "engine/editor/editor.hpp"
#include "engine/renderer/imgui_pass.hpp"
#endif
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/rhi/swapchain.hpp"

namespace {

constexpr int  k_window_width  = 1280;
constexpr int  k_window_height = 720;
constexpr char k_window_title[] = "quat-3d";
constexpr std::uint32_t k_frames_in_flight = 2;

#ifndef NDEBUG
constexpr bool k_enable_validation = true;
#else
constexpr bool k_enable_validation = false;
#endif

VkExtent2D window_pixel_extent(SDL_Window* window) {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return VkExtent2D{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

// Records the triangle pass into `cmd`: dynamic-rendering scope clearing the
// colour attachment to black, then a 3-vertex draw of the hard-coded triangle.
// The image is already in COLOR_ATTACHMENT_OPTIMAL (the render graph inserted
// the barrier) and is transitioned to PRESENT_SRC by the graph afterwards.
void record_triangle(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent,
                     VkPipeline pipeline) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &rendering);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

// Offscreen verification of the triangle (display-independent). Renders the
// triangle into a transient colour image via the render graph, copies it to a
// host buffer, and checks the centre pixel (inside the triangle) is non-black
// while a corner (outside it) stays at the clear colour. Confirms the pipeline,
// dynamic rendering, and barrier insertion actually produce pixels.
std::expected<void, engine::core::Error>
run_triangle_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                       const std::string& mesh_spv) {
    using engine::core::Error;
    constexpr VkExtent2D extent{64, 64};
    constexpr VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto cache = engine::rhi::PipelineCache::create(device);
    if (!cache) return std::unexpected(cache.error());
    auto shader = cache->load(mesh_spv);
    if (!shader) return std::unexpected(shader.error());

    auto pipeline = engine::rhi::GraphicsPipeline::create(
        device, cache->handle(),
        {.vertex = *shader, .fragment = *shader, .color_formats = {&fmt, 1}});
    if (!pipeline) return std::unexpected(pipeline.error());

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();
    const VkPipeline vk_pipeline = pipeline->handle();

    engine::rhi::TransientImagePool pool(device, allocator);
    engine::rhi::RenderGraph graph(pool);
    const engine::rhi::ResourceHandle color = graph.create_transient_image(
        "selftest_color", fmt, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    graph.add_pass("draw", engine::rhi::PassType::graphics)
        .writes(color, engine::rhi::ResourceUsage::color_attachment)
        .execute([=](engine::rhi::PassContext& ctx) {
            const auto b = ctx.resolve(color);
            record_triangle(ctx.cmd(), b.view, extent, vk_pipeline);
        });
    graph.add_pass("copy", engine::rhi::PassType::transfer)
        .reads(color, engine::rhi::ResourceUsage::transfer_src)
        .execute([=](engine::rhi::PassContext& ctx) {
            const auto b = ctx.resolve(color);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(ctx.cmd(), b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback_buffer, 1, &region);
        });
    if (auto r = graph.compile(); !r) return std::unexpected(r.error());

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pool_info, nullptr, &cmd_pool);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    graph.execute(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    std::expected<void, Error> result{};
    if (vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        result = std::unexpected(Error{"triangle self-test: vkQueueSubmit2 failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        std::vector<std::uint8_t> pixels(bytes);
        std::memcpy(pixels.data(), readback->mapped(), bytes);
        auto luma = [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t o = (static_cast<std::size_t>(y) * extent.width + x) * 4;
            return static_cast<int>(pixels[o]) + pixels[o + 1] + pixels[o + 2];
        };
        const int centre = luma(extent.width / 2, extent.height / 2);
        const int corner = luma(0, 0);
        if (centre <= 30) {
            result = std::unexpected(Error{"triangle self-test: centre pixel is black (no triangle)"});
        } else if (corner > 30) {
            result = std::unexpected(Error{"triangle self-test: corner pixel not clear-black"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

// Per-frame-in-flight bundle for the live deferred chain. Each frame slot owns
// its own pass instances (which hold per-frame descriptor buffers) and transient
// image pool, so building frame N+1's graph never stomps resources still in use
// by frame N. The main loop waits on the slot's fence before reusing it, which
// makes resetting the pool and rebuilding the descriptor buffers safe.
// Per-skinned-entity GPU buffers for one frame slot: the joint matrices (host
// visible, re-uploaded each frame) and the skinned vertex output (device local,
// written by the skinning pass and read by the GBuffer as a vertex buffer).
struct SkinnedEntityGpu {
    engine::rhi::GpuBuffer joints;
    engine::rhi::GpuBuffer skinned_verts;
    VkDeviceAddress        joints_addr = 0;
    VkDeviceAddress        skinned_addr = 0;
    std::uint32_t          vertex_count = 0;
    std::uint32_t          joint_count = 0;
};

struct DeferredFrame {
    engine::renderer::MeshPass       mesh;
    engine::renderer::LightingPass   lighting;
    engine::renderer::TonemapPass    tonemap;
    engine::renderer::SkinningPass   skinning;
    engine::renderer::ShadowPass     shadow;
    engine::renderer::TransparentPass transparent;
    engine::renderer::BloomPass      bloom;
    engine::renderer::ExposurePass   exposure;
    engine::renderer::TaaPass        taa;
#ifdef ENGINE_EDITOR
    engine::renderer::ImGuiPass      imgui;
    // Offscreen scene target for the editor's Viewport panel: TAA resolves the
    // frame here (instead of the swapchain) and the UI samples it. Recreated
    // when the panel is resized.
    engine::rhi::GpuImage            viewport_image;
    engine::rhi::ImageView           viewport_view;
    VkExtent2D                       viewport_extent{0, 0};
#endif
    engine::rhi::TransientImagePool  pool;
    // Skinning resources, lazily created per skinned entity (keyed by entity).
    std::unordered_map<entt::entity, SkinnedEntityGpu> skin_gpu;
};

#ifdef ENGINE_EDITOR
// (Re)creates the slot's offscreen viewport image at `extent`. Safe to replace
// here: the slot's fence was waited before its frame is rebuilt. The image uses
// the swapchain format so the TAA resolve copy and the UI's sRGB sampling
// behave exactly as the direct-to-swapchain path.
bool ensure_viewport_target(DeferredFrame& frame, const engine::rhi::Device& device,
                            engine::rhi::GpuAllocator& allocator, VkFormat format,
                            VkExtent2D extent) {
    if (frame.viewport_image.handle() != VK_NULL_HANDLE &&
        frame.viewport_extent.width == extent.width &&
        frame.viewport_extent.height == extent.height) {
        return true;
    }
    auto image = allocator.create_image(format, extent,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!image) return false;
    auto view = engine::rhi::create_image_view(device.handle(), image->handle(), format);
    if (!view) return false;
    frame.viewport_image = std::move(*image);
    frame.viewport_view = std::move(*view);
    frame.viewport_extent = extent;
    return true;
}
#endif

// Fetch (or create) this frame slot's skinning buffers for `entity`. Returns
// nullptr if allocation fails (the caller then renders the bind pose).
SkinnedEntityGpu* ensure_skin_gpu(DeferredFrame& frame, entt::entity entity,
                                  const engine::asset::MeshAsset& mesh, VkDevice vk_device,
                                  engine::rhi::GpuAllocator& allocator, std::uint32_t joint_count) {
    auto it = frame.skin_gpu.find(entity);
    if (it != frame.skin_gpu.end() && it->second.vertex_count == mesh.vertex_count &&
        it->second.joint_count == joint_count) {
        return &it->second;
    }

    const VkDeviceSize joints_size = static_cast<VkDeviceSize>(joint_count) * sizeof(glm::mat4);
    const VkDeviceSize verts_size =
        static_cast<VkDeviceSize>(mesh.vertex_count) * sizeof(engine::asset::Vertex);

    auto joints = allocator.create_buffer(
        joints_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!joints) return nullptr;
    auto verts = allocator.create_buffer(
        verts_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!verts) return nullptr;

    SkinnedEntityGpu g;
    g.joints = std::move(*joints);
    g.skinned_verts = std::move(*verts);
    g.joints_addr = engine::rhi::buffer_device_address(vk_device, g.joints);
    g.skinned_addr = engine::rhi::buffer_device_address(vk_device, g.skinned_verts);
    g.vertex_count = mesh.vertex_count;
    g.joint_count = joint_count;
    frame.skin_gpu[entity] = std::move(g);
    return &frame.skin_gpu[entity];
}

// A unit cube centred at the origin with per-face normals (24 vertices, 36
// indices). Stands in as the demo mesh until glTF scene-graph loading (4.4).
engine::scene::MeshData make_cube_data() {
    using engine::asset::Vertex;
    engine::scene::MeshData d;
    const glm::vec3 face_normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const glm::vec3& n : face_normals) {
        const glm::vec3 up = (std::abs(n.y) > 0.9F) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 t = glm::normalize(glm::cross(up, n));
        const glm::vec3 b = glm::cross(n, t);
        const glm::vec3 c = n * 0.5F;
        const glm::vec3 corners[4] = {
            c - t * 0.5F - b * 0.5F, c + t * 0.5F - b * 0.5F,
            c + t * 0.5F + b * 0.5F, c - t * 0.5F + b * 0.5F};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const auto base = static_cast<std::uint32_t>(d.vertices.size());
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.position = corners[i];
            v.normal = n;
            v.uv = uvs[i];
            v.tangent = glm::vec4(t, 1.0F);
            d.vertices.push_back(v);
        }
        for (std::uint32_t idx : {base, base + 1, base + 2, base, base + 2, base + 3}) {
            d.indices.push_back(idx);
        }
    }
    d.bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    d.submeshes = {engine::asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
    return d;
}

// A box with the given half-extents (the unit cube scaled into geometry, so the
// mesh carries no Transform scale — keeps physics rotation extraction clean).
engine::scene::MeshData make_box_data(const glm::vec3& half_extents) {
    engine::scene::MeshData d = make_cube_data();
    const glm::vec3 s = half_extents / 0.5F;
    for (engine::asset::Vertex& v : d.vertices) {
        v.position *= s;
    }
    d.bounds = {-half_extents, half_extents};
    return d;
}

// A unit-radius UV sphere (latitude/longitude grid). Good for showing off PBR
// metallic/roughness variation in the showcase scene.
engine::scene::MeshData make_sphere_data(float radius, std::uint32_t rings,
                                         std::uint32_t sectors) {
    using engine::asset::Vertex;
    engine::scene::MeshData d;
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>(); // 0..pi (pole to pole)
        const float y = std::cos(phi);
        const float ring_r = std::sin(phi);
        for (std::uint32_t s = 0; s <= sectors; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(sectors);
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 n{ring_r * std::cos(theta), y, ring_r * std::sin(theta)};
            Vertex vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = {u, v};
            // Tangent along +theta (east).
            vert.tangent = glm::vec4(-std::sin(theta), 0.0F, std::cos(theta), 1.0F);
            d.vertices.push_back(vert);
        }
    }
    const std::uint32_t stride = sectors + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < sectors; ++s) {
            const std::uint32_t i0 = r * stride + s;
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + stride;
            const std::uint32_t i3 = i2 + 1;
            for (std::uint32_t idx : {i0, i2, i1, i1, i2, i3}) d.indices.push_back(idx);
        }
    }
    d.bounds = {glm::vec3(-radius), glm::vec3(radius)};
    d.submeshes = {engine::asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
    return d;
}

// World-space AABB over every loaded renderable (mesh local bounds transformed by
// its world matrix). Returns false if the scene has no loaded geometry. Tick the
// scene first so world transforms are current.
bool scene_world_bounds(engine::scene::Scene& scene, glm::vec3& out_min, glm::vec3& out_max) {
    out_min = glm::vec3(std::numeric_limits<float>::max());
    out_max = glm::vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    for (auto [e, t, mr] :
         scene.registry().view<engine::scene::Transform, engine::scene::MeshRenderer>().each()) {
        if (!mr.mesh.valid() || !mr.mesh.is_loaded()) continue;
        // Skinned meshes render with an identity model (their skinning matrices,
        // which fold in the armature transform, already place the vertices), so
        // frame their bind bounds through that same armature transform.
        const auto* sm = scene.registry().try_get<engine::scene::SkinnedMesh>(e);
        const glm::mat4 model = sm != nullptr ? sm->root_transform : t.world;
        const engine::asset::Aabb& ab = mr.mesh->bounds;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? ab.max.x : ab.min.x, (i & 2) ? ab.max.y : ab.min.y,
                                   (i & 4) ? ab.max.z : ab.min.z);
            const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0F));
            out_min = glm::min(out_min, w);
            out_max = glm::max(out_max, w);
            any = true;
        }
    }
    return any;
}

// Camera world transform that frames the given AABB: looks at its centre from a
// distance sized so the bounding sphere fits the vertical field of view.
glm::mat4 frame_camera_world(const glm::vec3& bmin, const glm::vec3& bmax, float fov_y) {
    const glm::vec3 center = 0.5F * (bmin + bmax);
    const float radius = glm::max(0.001F, 0.5F * glm::length(bmax - bmin));
    const float dist = radius / std::tan(fov_y * 0.5F) * 1.6F;
    const glm::vec3 eye = center + glm::normalize(glm::vec3(0.6F, 0.45F, 1.0F)) * dist;
    return glm::inverse(glm::lookAt(eye, center, glm::vec3(0.0F, 1.0F, 0.0F)));
}

// Offscreen render of the demo scene (cube + perspective camera + sun) through
// the full deferred chain into an LDR image, read back to verify the camera
// frames the cube: the centre pixel (on the cube) is lit and a corner pixel
// (background) stays black. Unlike the Phase 3 chain self-tests this drives the
// real camera_system matrices, so it catches a broken projection / view / Y-flip
// that "runs clean" would miss.
std::expected<void, engine::core::Error>
run_scene_render_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                           engine::rhi::PipelineCache& cache,
                           const engine::rhi::TransferContext& transfer,
                           const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto mesh = scene::upload_mesh(make_cube_data(), allocator, transfer);
    if (!mesh) return std::unexpected(mesh.error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.85F, 0.35F, 0.2F, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 0.6F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting_pass = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    if (!lighting_pass) return std::unexpected(lighting_pass.error());
    auto tonemap_pass =
        renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!tonemap_pass) return std::unexpected(tonemap_pass.error());

    // Camera framing the cube; aspect 1 for the square render target.
    scene::Scene s;
    const entt::entity cam_e = s.create_entity("camera");
    s.registry().get<scene::Transform>(cam_e).local =
        glm::inverse(glm::lookAt(glm::vec3(3.0F, 2.5F, 4.0F), glm::vec3(0.0F),
                                 glm::vec3(0.0F, 1.0F, 0.0F)));
    s.registry().emplace<scene::Camera>(cam_e);
    s.tick();
    const scene::CameraMatrices cam = scene::camera_system(s.registry(), 1.0F);

    renderer::DirectionalLightParams light;
    light.direction = glm::vec4(-glm::normalize(glm::vec3(-0.4F, -1.0F, -0.3F)), 0.0F);
    light.color = glm::vec4(1.0F, 0.98F, 0.95F, 3.0F);

    const renderer::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, extent, cam.view_proj, {&item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hdr = lighting_pass->add_to_graph(graph, *gbuffer, extent, light,
                                           glm::inverse(cam.view_proj), cam.position,
                                           engine::rhi::ResourceHandle{}, glm::mat4(1.0F), {},
                                           /*enable_sky=*/true);
    if (!hdr) return std::unexpected(hdr.error());
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "scene_ldr", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto r = tonemap_pass->add_to_graph(graph, *hdr, ldr, extent); !r) {
        return std::unexpected(r.error());
    }
    if (auto compiled = graph.compile(); !compiled) return std::unexpected(compiled.error());

    const VkImage ldr_image = graph.binding(ldr).image;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pool_info, nullptr, &cmd_pool);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    graph.execute(cmd);

    // Tonemap left the LDR image in COLOR_ATTACHMENT_OPTIMAL; move it to
    // TRANSFER_SRC for the read-back copy.
    VkImageMemoryBarrier2 to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = ldr_image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                           &region);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    std::expected<void, core::Error> result{};
    if (vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        result = std::unexpected(core::Error{"scene render self-test: vkQueueSubmit2 failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const auto chan = [&](std::uint32_t x, std::uint32_t y, int c) {
            return static_cast<int>(px[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
        };
        const auto luma = [&](std::uint32_t x, std::uint32_t y) {
            return chan(x, y, 0) + chan(x, y, 1) + chan(x, y, 2);
        };
        if (luma(extent.width / 2, extent.height / 2) <= 30) {
            result = std::unexpected(
                core::Error{"scene render self-test: cube centre is black (camera not framing it)"});
        } else if (luma(2, 2) <= 60 || chan(2, 2, 2) <= chan(2, 2, 0)) {
            // The corner is sky now: it must be bright and blue-dominant.
            result = std::unexpected(
                core::Error{"scene render self-test: sky background missing or not blue"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

// Renders a box hovering over a floor, once with shadows and once without, and
// verifies the shadowed image is darker overall — i.e. the shadow map actually
// removes light from the floor (a no-op shadow would leave the two identical).
std::expected<void, engine::core::Error>
run_shadow_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                     engine::rhi::PipelineCache& cache,
                     const engine::rhi::TransferContext& transfer, const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto floor = scene::upload_mesh(make_box_data(glm::vec3(4.0F, 0.25F, 4.0F)), allocator, transfer);
    auto block = scene::upload_mesh(make_box_data(glm::vec3(0.7F)), allocator, transfer);
    if (!floor || !block) return std::unexpected((!floor ? floor : block).error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.8F, 0.8F, 0.8F, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 0.9F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    auto lighting = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    auto shadow_pass = renderer::ShadowPass::create(device, cache, shader_dir);
    auto tonemap = renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!mesh_pass || !lighting || !shadow_pass || !tonemap) {
        return std::unexpected(core::Error{"shadow self-test: pass creation failed"});
    }

    // Angled camera looking down at the floor; an overhead light tilted slightly.
    const glm::vec3 eye(4.0F, 5.0F, 5.0F);
    glm::mat4 proj = glm::perspective(glm::radians(50.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 view_proj = proj * glm::lookAt(eye, glm::vec3(0.0F, 0.5F, 0.0F),
                                                   glm::vec3(0.0F, 1.0F, 0.0F));

    const glm::vec3 travel = glm::normalize(glm::vec3(0.15F, -1.0F, 0.1F));
    renderer::DirectionalLightParams light;
    light.direction = glm::vec4(-travel, 0.0F);
    light.color = glm::vec4(1.0F, 1.0F, 1.0F, 3.0F);
    light.ambient = glm::vec4(0.05F, 0.05F, 0.05F, 0.0F);

    const glm::vec3 lcenter(0.0F, 1.0F, 0.0F);
    constexpr float lr = 5.0F;
    const glm::mat4 light_vp =
        glm::ortho(-lr, lr, -lr, lr, 0.1F, lr * 3.5F) *
        glm::lookAt(lcenter - travel * (lr * 1.5F), lcenter, glm::vec3(0.0F, 1.0F, 0.0F));

    const std::array<renderer::DrawItem, 2> draws{
        renderer::DrawItem{&*floor, &*material, glm::mat4(1.0F)},
        renderer::DrawItem{&*block, &*material,
                           glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.6F, 0.0F))}};

    const auto render_luma = [&](bool use_shadow) -> std::expected<long long, core::Error> {
        auto readback = allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!readback) return std::unexpected(readback.error());
        const VkBuffer readback_buffer = readback->handle();

        rhi::TransientImagePool pool(device, allocator);
        rhi::RenderGraph graph(pool);
        rhi::ResourceHandle shadow_handle{};
        if (use_shadow) {
            auto s = shadow_pass->add_to_graph(graph, light_vp, draws);
            if (!s) return std::unexpected(s.error());
            shadow_handle = *s;
        }
        auto gbuffer = mesh_pass->add_to_graph(graph, extent, view_proj, draws);
        if (!gbuffer) return std::unexpected(gbuffer.error());
        auto hdr = lighting->add_to_graph(graph, *gbuffer, extent, light, glm::inverse(view_proj),
                                          eye, shadow_handle, light_vp);
        if (!hdr) return std::unexpected(hdr.error());
        const rhi::ResourceHandle ldr = graph.create_transient_image(
            "shadow_ldr", ldr_format, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (auto r = tonemap->add_to_graph(graph, *hdr, ldr, extent); !r) {
            return std::unexpected(r.error());
        }
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());
        const VkImage ldr_image = graph.binding(ldr).image;

        VkCommandPool pool_h = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(device.handle(), &pi, nullptr, &pool_h);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_h;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        graph.execute(cmd);
        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = ldr_image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_src;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buffer, 1, &region);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        ci.commandBuffer = cmd;
        VkSubmitInfo2 si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &ci;
        const VkResult sr = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
        long long sum = 0;
        if (sr == VK_SUCCESS) {
            vkQueueWaitIdle(device.graphics_queue());
            const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
            for (VkDeviceSize i = 0; i < bytes; i += 4) {
                sum += static_cast<long long>(px[i]) + px[i + 1] + px[i + 2];
            }
        }
        vkDestroyCommandPool(device.handle(), pool_h, nullptr);
        if (sr != VK_SUCCESS) return std::unexpected(core::Error{"shadow self-test: submit failed"});
        return sum;
    };

    auto unshadowed = render_luma(false);
    if (!unshadowed) return std::unexpected(unshadowed.error());
    auto shadowed = render_luma(true);
    if (!shadowed) return std::unexpected(shadowed.error());

    // Shadows can only remove light, so an effective shadow map makes the floor
    // strictly darker; a no-op shadow would leave the two images identical.
    if (*shadowed >= *unshadowed) {
        return std::unexpected(core::Error{"shadow self-test: shadows did not darken the scene"});
    }
    return {};
}

// Renders an opaque red wall with a half-transparent blue panel in front and
// verifies the panel pixel is a blend (both red showing through and blue added),
// while a pixel off the panel stays red — proving alpha blending works.
std::expected<void, engine::core::Error>
run_transparent_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                          engine::rhi::PipelineCache& cache,
                          const engine::rhi::TransferContext& transfer,
                          const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto wall = scene::upload_mesh(make_box_data(glm::vec3(3.0F, 3.0F, 0.2F)), allocator, transfer);
    auto panel = scene::upload_mesh(make_box_data(glm::vec3(1.0F, 1.0F, 0.05F)), allocator, transfer);
    if (!wall || !panel) return std::unexpected((!wall ? wall : panel).error());

    asset::PbrMaterialParams red_params;
    red_params.base_color_factor = {0.9F, 0.1F, 0.1F, 1.0F};
    red_params.roughness_factor = 0.9F;
    auto red = asset::upload_material(red_params, asset::MaterialTextures{}, allocator, transfer);
    asset::PbrMaterialParams blue_params;
    blue_params.base_color_factor = {0.1F, 0.2F, 0.95F, 0.5F};
    blue_params.roughness_factor = 0.5F;
    blue_params.flags = asset::material_blend;
    auto blue = asset::upload_material(blue_params, asset::MaterialTextures{}, allocator, transfer);
    if (!red || !blue) return std::unexpected((!red ? red : blue).error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    auto lighting = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    auto transparent =
        renderer::TransparentPass::create(device, allocator, cache, transfer, shader_dir);
    auto tonemap = renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!mesh_pass || !lighting || !transparent || !tonemap) {
        return std::unexpected(core::Error{"transparent self-test: pass creation failed"});
    }

    const glm::vec3 eye(0.0F, 0.0F, 3.5F);
    glm::mat4 proj = glm::perspective(glm::radians(45.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 view_proj = proj * glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    renderer::DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 3.0F};
    light.ambient = {0.1F, 0.1F, 0.1F, 0.0F};

    const renderer::DrawItem wall_item{&*wall, &*red,
                                       glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -1.0F))};
    const renderer::DrawItem panel_item{&*panel, &*blue, glm::mat4(1.0F)};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, extent, view_proj, {&wall_item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hdr = lighting->add_to_graph(graph, *gbuffer, extent, light, glm::inverse(view_proj), eye,
                                      rhi::ResourceHandle{}, glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());
    if (auto r = transparent->add_to_graph(graph, *hdr, gbuffer->depth, extent, view_proj, light,
                                           eye, {&panel_item, 1});
        !r) {
        return std::unexpected(r.error());
    }
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "t_ldr", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto r = tonemap->add_to_graph(graph, *hdr, ldr, extent); !r) {
        return std::unexpected(r.error());
    }
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    const VkImage ldr_image = graph.binding(ldr).image;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pi, nullptr, &cmd_pool);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    graph.execute(cmd);
    VkImageMemoryBarrier2 to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = ldr_image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                           &region);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult sr = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    std::expected<void, core::Error> result{};
    if (sr != VK_SUCCESS) {
        result = std::unexpected(core::Error{"transparent self-test: submit failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const auto chan = [&](std::uint32_t x, std::uint32_t y, int c) {
            return static_cast<int>(px[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
        };
        // Centre is behind the panel: blend of red wall + blue panel -> both present.
        const std::uint32_t cx = extent.width / 2;
        const std::uint32_t cy = extent.height / 2;
        if (chan(cx, cy, 0) <= 25 || chan(cx, cy, 2) <= 25) {
            result = std::unexpected(
                core::Error{"transparent self-test: panel pixel is not a red/blue blend"});
        } else if (chan(6, cy, 0) <= chan(6, cy, 2)) {
            // Off the panel (left edge) the wall is plain red: red > blue.
            result = std::unexpected(
                core::Error{"transparent self-test: off-panel wall is not red"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

// Glass / transmission (7.5): a bright white wall behind a blue-tinted transmission
// panel. The panel must show the wall *through* it, tinted blue — so a pixel behind
// the panel is blue-dominant, while off the panel the wall reads plain white.
[[nodiscard]] std::expected<void, engine::core::Error>
run_glass_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                    engine::rhi::PipelineCache& cache,
                    const engine::rhi::TransferContext& transfer, const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto wall = scene::upload_mesh(make_box_data(glm::vec3(3.0F, 3.0F, 0.2F)), allocator, transfer);
    auto panel = scene::upload_mesh(make_box_data(glm::vec3(1.0F, 1.0F, 0.05F)), allocator, transfer);
    if (!wall || !panel) return std::unexpected((!wall ? wall : panel).error());

    asset::PbrMaterialParams white_params;
    white_params.base_color_factor = {1.0F, 1.0F, 1.0F, 1.0F};
    white_params.roughness_factor = 0.9F;
    auto white = asset::upload_material(white_params, asset::MaterialTextures{}, allocator, transfer);
    asset::PbrMaterialParams glass_params;
    glass_params.base_color_factor = {0.1F, 0.25F, 1.0F, 1.0F}; // blue tint
    glass_params.transmission_factor = 1.0F;
    glass_params.roughness_factor = 0.8F; // damp specular
    glass_params.flags = asset::material_transmission;
    auto glass = asset::upload_material(glass_params, asset::MaterialTextures{}, allocator, transfer);
    if (!white || !glass) return std::unexpected((!white ? white : glass).error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    auto lighting = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    auto transparent =
        renderer::TransparentPass::create(device, allocator, cache, transfer, shader_dir);
    auto tonemap = renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!mesh_pass || !lighting || !transparent || !tonemap) {
        return std::unexpected(core::Error{"glass self-test: pass creation failed"});
    }

    const glm::vec3 eye(0.0F, 0.0F, 3.5F);
    glm::mat4 proj = glm::perspective(glm::radians(45.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 view_proj = proj * glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    renderer::DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 1.5F};
    light.ambient = {0.2F, 0.2F, 0.2F, 0.0F};

    const renderer::DrawItem wall_item{&*wall, &*white,
                                       glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -1.0F))};
    const renderer::DrawItem panel_item{&*panel, &*glass, glm::mat4(1.0F)};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, extent, view_proj, {&wall_item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hdr = lighting->add_to_graph(graph, *gbuffer, extent, light, glm::inverse(view_proj), eye,
                                      rhi::ResourceHandle{}, glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());
    if (auto r = transparent->add_to_graph(graph, *hdr, gbuffer->depth, extent, view_proj, light,
                                           eye, {&panel_item, 1});
        !r) {
        return std::unexpected(r.error());
    }
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "g_ldr", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto r = tonemap->add_to_graph(graph, *hdr, ldr, extent); !r) {
        return std::unexpected(r.error());
    }
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    const VkImage ldr_image = graph.binding(ldr).image;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pi, nullptr, &cmd_pool);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    graph.execute(cmd);
    VkImageMemoryBarrier2 to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = ldr_image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                           &region);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult sr = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    std::expected<void, core::Error> result{};
    if (sr != VK_SUCCESS) {
        result = std::unexpected(core::Error{"glass self-test: submit failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const auto chan = [&](std::uint32_t x, std::uint32_t y, int c) {
            return static_cast<int>(px[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
        };
        const std::uint32_t cy = extent.height / 2;
        const std::uint32_t px_panel = extent.width / 2 - 18; // behind panel, off the highlight
        const int pr = chan(px_panel, cy, 0);
        const int pb = chan(px_panel, cy, 2);
        const int wr = chan(6, cy, 0);
        const int wb = chan(6, cy, 2);
        if (std::getenv("QUAT_GLASS_DEBUG") != nullptr) {
            std::fprintf(stderr, "[glass] panel rgb=%d,%d,%d  wall rgb=%d,%d,%d\n", pr,
                         chan(px_panel, cy, 1), pb, wr, chan(6, cy, 1), wb);
        }
        if (wr <= 60 || wb <= 60) {
            result = std::unexpected(core::Error{"glass self-test: wall is not lit white"});
        } else if (pb <= 40) {
            result =
                std::unexpected(core::Error{"glass self-test: nothing transmitted through glass"});
        } else if (pb <= pr + 15) {
            result = std::unexpected(
                core::Error{"glass self-test: transmitted wall is not blue-tinted by the glass"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        k_window_title, k_window_width, k_window_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (sdl_extensions == nullptr) {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = k_window_title,
    });
    if (!device_result) {
        std::fprintf(stderr, "[fatal] device creation failed: %s\n",
                     device_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Device& device = *device_result;
    const VkDevice vk_device = device.handle();

    auto allocator_result = engine::rhi::GpuAllocator::create(device);
    if (!allocator_result) {
        std::fprintf(stderr, "[fatal] allocator creation failed: %s\n",
                     allocator_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::GpuAllocator& allocator = *allocator_result;

    if (std::getenv("QUAT_SELFTEST") != nullptr) {
        if (auto r = engine::asset::run_asset_manager_self_test(); r) {
            std::fprintf(stderr, "[selftest] asset manager OK\n");
        } else {
            std::fprintf(stderr, "[selftest] asset manager FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_ecs_self_test(); r) {
            std::fprintf(stderr, "[selftest] ECS world OK\n");
        } else {
            std::fprintf(stderr, "[selftest] ECS world FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_scene_self_test(); r) {
            std::fprintf(stderr, "[selftest] scene + systems OK\n");
        } else {
            std::fprintf(stderr, "[selftest] scene + systems FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::animation::run_skeleton_self_test(); r) {
            std::fprintf(stderr, "[selftest] skeleton runtime OK\n");
        } else {
            std::fprintf(stderr, "[selftest] skeleton runtime FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::animation::run_clip_self_test(); r) {
            std::fprintf(stderr, "[selftest] clip sampling OK\n");
        } else {
            std::fprintf(stderr, "[selftest] clip sampling FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::animation::run_animator_self_test(); r) {
            std::fprintf(stderr, "[selftest] animator OK\n");
        } else {
            std::fprintf(stderr, "[selftest] animator FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::animation::run_blend_self_test(); r) {
            std::fprintf(stderr, "[selftest] blend + additive OK\n");
        } else {
            std::fprintf(stderr, "[selftest] blend + additive FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::animation::run_state_machine_self_test(); r) {
            std::fprintf(stderr, "[selftest] state machine OK\n");
        } else {
            std::fprintf(stderr, "[selftest] state machine FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::physics::run_physics_self_test(); r) {
            std::fprintf(stderr, "[selftest] physics world OK\n");
        } else {
            std::fprintf(stderr, "[selftest] physics world FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_physics_body_self_test(); r) {
            std::fprintf(stderr, "[selftest] physics bodies + ECS sync OK\n");
        } else {
            std::fprintf(stderr, "[selftest] physics bodies + ECS sync FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_character_self_test(); r) {
            std::fprintf(stderr, "[selftest] character controller OK\n");
        } else {
            std::fprintf(stderr, "[selftest] character controller FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::physics::run_trigger_self_test(); r) {
            std::fprintf(stderr, "[selftest] trigger events OK\n");
        } else {
            std::fprintf(stderr, "[selftest] trigger events FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_skeleton_load_self_test(); r) {
            std::fprintf(stderr, "[selftest] skeleton load OK\n");
        } else {
            std::fprintf(stderr, "[selftest] skeleton load FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_animation_load_self_test(); r) {
            std::fprintf(stderr, "[selftest] animation load OK\n");
        } else {
            std::fprintf(stderr, "[selftest] animation load FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::rhi::run_gpu_round_trip_self_test(device, allocator); r) {
            std::fprintf(stderr, "[selftest] VMA GPU round-trip OK\n");
        } else {
            std::fprintf(stderr, "[selftest] FAILED: %s\n", r.error().message.c_str());
        }

        if (auto r = engine::rhi::run_descriptor_buffer_self_test(device, allocator); r) {
            std::fprintf(stderr, "[selftest] descriptor buffer OK\n");
        } else {
            std::fprintf(stderr, "[selftest] descriptor buffer FAILED: %s\n",
                         r.error().message.c_str());
        }

        {
            engine::rhi::TransientImagePool pool(device, allocator);
            if (auto r = engine::rhi::run_render_graph_self_test(device, allocator, pool); r) {
                std::fprintf(stderr, "[selftest] render graph OK\n");
            } else {
                std::fprintf(stderr, "[selftest] render graph FAILED: %s\n",
                             r.error().message.c_str());
            }
        }

        {
            const std::string mesh_spv = std::string(QUAT_COOKED_SHADER_DIR) + "/mesh.spv";
            if (auto r = run_triangle_self_test(device, allocator, mesh_spv); r) {
                std::fprintf(stderr, "[selftest] triangle render OK\n");
            } else {
                std::fprintf(stderr, "[selftest] triangle render FAILED: %s\n",
                             r.error().message.c_str());
            }
        }

        {
            // Transient transfer context for the glTF upload path.
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            // Graphics family: mip-generation blits (vkCmdBlitImage) are only
            // legal on a graphics-capable queue.
            pool_info.queueFamilyIndex = device.queue_families().graphics;
            if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext transfer{
                    device.handle(), pool, device.graphics_queue(),
                    device.max_sampler_anisotropy()};
                if (auto r = engine::scene::run_gltf_loader_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] glTF loader OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] glTF loader FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::scene::run_texture_loader_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] texture loader OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] texture loader FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::asset::run_material_asset_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] material asset OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] material asset FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::scene::run_material_extract_self_test(); r) {
                    std::fprintf(stderr, "[selftest] material extract OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] material extract FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::scene::run_gltf_scene_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] glTF scene graph OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] glTF scene graph FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto mesh_cache = engine::rhi::PipelineCache::create(device); mesh_cache) {
                    const std::string shader_dir = QUAT_COOKED_SHADER_DIR;
                    if (auto r = engine::renderer::run_mesh_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] GBuffer pass OK\n");
                    } else {
                        std::fprintf(stderr, "[selftest] GBuffer pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_lighting_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] lighting pass OK\n");
                    } else {
                        std::fprintf(stderr, "[selftest] lighting pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_point_light_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] point lights OK\n");
                    } else {
                        std::fprintf(stderr, "[selftest] point lights FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_clearcoat_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] clearcoat OK (coat specular lobe)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] clearcoat FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_specular_aa_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] specular AA OK (variance widens roughness)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] specular AA FAILED: %s\n",
                                     r.error().message.c_str());
                    }
#ifdef ENGINE_EDITOR
                    if (auto r = engine::renderer::run_imgui_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] imgui pass OK (UI over cleared target)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] imgui pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
#endif
                    if (auto r = engine::renderer::run_ibl_self_test(device, allocator, *mesh_cache,
                                                                     shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] IBL OK (irradiance/prefilter/BRDF LUT)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] IBL FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_ibl_lighting_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] IBL lighting OK (metal reflects sky)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] IBL lighting FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_bloom_self_test(device, allocator, *mesh_cache,
                                                                       shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] bloom OK (highlights glow)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] bloom FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_exposure_self_test(device, allocator,
                                                                          *mesh_cache, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] exposure OK (adapts to brightness)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] exposure FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_taa_self_test(device, allocator, *mesh_cache,
                                                                     shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] TAA OK (clamp rejects ghosting)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] TAA FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_tonemap_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] tonemap pass OK (full deferred chain)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] tonemap pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = run_scene_render_self_test(device, allocator, *mesh_cache, transfer,
                                                            shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] scene render OK (camera frames the cube)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] scene render FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_skinning_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] GPU skinning OK\n");
                    } else {
                        std::fprintf(stderr, "[selftest] GPU skinning FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = run_shadow_self_test(device, allocator, *mesh_cache, transfer,
                                                      shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] shadow map OK (floor darkened)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] shadow map FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = run_transparent_self_test(device, allocator, *mesh_cache, transfer,
                                                           shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] transparency OK (alpha blend)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] transparency FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = run_glass_self_test(device, allocator, *mesh_cache, transfer,
                                                     shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] glass OK (transmission shows wall)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] glass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                }
                // Optional real-asset smoke test: QUAT_GLTF=<path> loads a glTF/GLB
                // mesh + its textures and reports counts. Exercises the real decode
                // paths (GLB bufferView images, external PNG/JPEG files).
                if (const char* gltf_path = std::getenv("QUAT_GLTF"); gltf_path != nullptr) {
                    if (auto mesh = engine::scene::GltfLoader::load(gltf_path, allocator, transfer);
                        mesh) {
                        std::fprintf(stderr,
                                     "[gltf] %s: %u vertices, %u indices, %zu submeshes\n",
                                     gltf_path, mesh->vertex_count, mesh->index_count,
                                     mesh->submeshes.size());
                    } else {
                        std::fprintf(stderr, "[gltf] mesh load FAILED: %s\n",
                                     mesh.error().message.c_str());
                    }
                    if (auto texs =
                            engine::scene::GltfLoader::load_textures(gltf_path, allocator, transfer);
                        texs) {
                        std::fprintf(stderr, "[gltf] %zu texture(s) loaded\n", texs->size());
                        for (std::size_t i = 0; i < texs->size(); ++i) {
                            std::fprintf(stderr, "[gltf]   tex %zu: %ux%u format=%d valid=%d\n", i,
                                         (*texs)[i].width, (*texs)[i].height,
                                         static_cast<int>((*texs)[i].format),
                                         static_cast<int>((*texs)[i].valid()));
                        }
                    } else {
                        std::fprintf(stderr, "[gltf] texture load FAILED: %s\n",
                                     texs.error().message.c_str());
                    }
                    if (auto mats = engine::scene::GltfLoader::load_material_data(gltf_path);
                        mats) {
                        std::fprintf(stderr, "[gltf] %zu material(s)\n", mats->size());
                        for (std::size_t i = 0; i < mats->size(); ++i) {
                            const auto& p = (*mats)[i].params;
                            std::fprintf(stderr,
                                         "[gltf]   mat %zu: baseColor=(%.2f,%.2f,%.2f,%.2f) "
                                         "metallic=%.2f roughness=%.2f flags=0x%x\n",
                                         i, p.base_color_factor.x, p.base_color_factor.y,
                                         p.base_color_factor.z, p.base_color_factor.w,
                                         p.metallic_factor, p.roughness_factor, p.flags);
                        }
                    } else {
                        std::fprintf(stderr, "[gltf] material load FAILED: %s\n",
                                     mats.error().message.c_str());
                    }
                    if (auto skels = engine::scene::GltfLoader::load_skeletons(gltf_path); skels) {
                        std::fprintf(stderr, "[gltf] %zu skin(s)\n", skels->size());
                        for (std::size_t i = 0; i < skels->size(); ++i) {
                            std::fprintf(stderr, "[gltf]   skin %zu: %zu joint(s)\n", i,
                                         (*skels)[i].joint_count());
                        }
                    } else {
                        std::fprintf(stderr, "[gltf] skeleton load FAILED: %s\n",
                                     skels.error().message.c_str());
                    }
                    if (auto clips = engine::scene::GltfLoader::load_animations(gltf_path); clips) {
                        std::fprintf(stderr, "[gltf] %zu animation(s)\n", clips->size());
                        for (std::size_t i = 0; i < clips->size(); ++i) {
                            std::fprintf(stderr,
                                         "[gltf]   clip %zu '%s': %.2fs, %zu channel(s)\n", i,
                                         (*clips)[i].name.c_str(), (*clips)[i].duration,
                                         (*clips)[i].channels.size());
                        }
                    } else {
                        std::fprintf(stderr, "[gltf] animation load FAILED: %s\n",
                                     clips.error().message.c_str());
                    }
                    // Instantiate into a scene and tick it, to confirm skinned
                    // entities get an Animator that poses their joint matrices.
                    {
                        engine::asset::AssetManager smoke_assets;
                        engine::scene::Scene smoke_scene;
                        if (auto roots = engine::scene::GltfLoader::instantiate(
                                gltf_path, allocator, transfer, smoke_assets, smoke_scene);
                            roots) {
                            smoke_scene.tick(0.5F); // runs the animation system
                            int skinned = 0;
                            bool posed = false;
                            for (auto [e, sm] :
                                 smoke_scene.registry().view<engine::scene::SkinnedMesh>().each()) {
                                ++skinned;
                                for (const glm::mat4& jm : sm.joint_matrices) {
                                    if (jm != glm::mat4(1.0F)) {
                                        posed = true;
                                        break;
                                    }
                                }
                            }
                            std::fprintf(stderr, "[gltf] %d skinned entity(ies), animated=%s\n",
                                         skinned, posed ? "yes" : "no");
                        } else {
                            std::fprintf(stderr, "[gltf] instantiate FAILED: %s\n",
                                         roots.error().message.c_str());
                        }
                    }
                }
                vkDestroyCommandPool(device.handle(), pool, nullptr);
            } else {
                std::fprintf(stderr, "[selftest] glTF loader FAILED: command pool creation\n");
            }
        }

        if (auto cache = engine::rhi::PipelineCache::create(device); cache) {
            const std::string spv = std::string(QUAT_COOKED_SHADER_DIR) + "/passthrough.spv";
            if (auto shader = cache->load(spv); shader) {
                const auto& refl = (*shader)->reflection;
                std::fprintf(stderr,
                    "[selftest] passthrough loaded: %zu entry points, "
                    "%zu push-constant ranges, %zu descriptor bindings\n",
                    refl.entry_points.size(), refl.push_constant_ranges.size(),
                    refl.descriptor_bindings.size());
                for (const auto& ep : refl.entry_points) {
                    std::fprintf(stderr, "[selftest]   entry: %s (stage 0x%x)\n",
                                 ep.name.c_str(), ep.stage);
                }
            } else {
                std::fprintf(stderr, "[selftest] shader load FAILED: %s\n",
                             shader.error().message.c_str());
            }
        } else {
            std::fprintf(stderr, "[selftest] pipeline cache FAILED: %s\n",
                         cache.error().message.c_str());
        }
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, device.instance(), nullptr, &surface)) {
        std::fprintf(stderr, "[fatal] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto swapchain_result =
        engine::rhi::Swapchain::create(device, surface, window_pixel_extent(window));
    if (!swapchain_result) {
        std::fprintf(stderr, "[fatal] swapchain creation failed: %s\n",
                     swapchain_result.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Swapchain& swapchain = *swapchain_result;

    // ---- Deferred renderer + demo scene ------------------------------------
    auto fatal = [&](const char* what, const std::string& detail) {
        std::fprintf(stderr, "[fatal] %s: %s\n", what, detail.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
    };

    auto pipeline_cache = engine::rhi::PipelineCache::create(device);
    if (!pipeline_cache) {
        fatal("pipeline cache creation failed", pipeline_cache.error().message);
        return 1;
    }

    const std::string shader_dir = QUAT_COOKED_SHADER_DIR;
    const VkFormat swap_format = swapchain.format();
    // The swapchain is usually sRGB, which cannot be a storage image (TAA writes
    // one) and would double-encode if the tonemap shader's explicit sRGB output
    // were hardware-encoded again. So tonemap + post-process work in the matching
    // UNORM format (the shader does the sRGB encode), then a size-compatible copy
    // moves the bits into the sRGB swapchain.
    const VkFormat ldr_format = [swap_format] {
        switch (swap_format) {
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            default: return swap_format;
        }
    }();

    // One-time transfer context for setup uploads (demo mesh/material + each
    // pass's fallback resources). Destroyed once setup completes.
    VkCommandPool setup_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // Graphics family: mip-generation blits (vkCmdBlitImage) are only legal
        // on a graphics-capable queue.
        info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(vk_device, &info, nullptr, &setup_pool);
    }
    const engine::rhi::TransferContext setup_transfer{vk_device, setup_pool,
                                                      device.graphics_queue(),
                                                      device.max_sampler_anisotropy()};

#ifdef ENGINE_EDITOR
    // Editor shell (ImGui context + SDL3 input backend). Created before the
    // frame slots: each slot's ImGuiPass uploads the font atlas of this
    // context. QUAT_NO_UI=1 disables it (deterministic headless renders).
    engine::editor::EditorLayer editor;
    engine::editor::RendererSettings render_settings;
    std::string pending_instantiate; // set by the asset browser, consumed below
    const bool editor_enabled = std::getenv("QUAT_NO_UI") == nullptr;
    if (editor_enabled) {
        if (auto e = engine::editor::EditorLayer::create(window); e) {
            editor = std::move(*e);
        } else {
            std::fprintf(stderr, "[warn] editor disabled: %s\n", e.error().message.c_str());
        }
    }
#endif

    // Per-frame-in-flight deferred chains (GBuffer -> lighting -> tonemap). Each
    // slot owns its passes' descriptor buffers and a transient image pool, so
    // building one frame's graph never disturbs the other in-flight frame.
    std::array<DeferredFrame, k_frames_in_flight> frames;
    bool frames_ok = true;
    for (auto& f : frames) {
        auto mesh = engine::renderer::MeshPass::create(device, allocator, *pipeline_cache,
                                                       setup_transfer, shader_dir);
        auto light = engine::renderer::LightingPass::create(device, allocator, *pipeline_cache,
                                                            shader_dir);
        auto tone = engine::renderer::TonemapPass::create(device, allocator, *pipeline_cache,
                                                          shader_dir, ldr_format);
        auto skin = engine::renderer::SkinningPass::create(device, allocator, *pipeline_cache,
                                                           shader_dir);
        auto shadow = engine::renderer::ShadowPass::create(device, *pipeline_cache, shader_dir);
        auto transp = engine::renderer::TransparentPass::create(device, allocator, *pipeline_cache,
                                                                setup_transfer, shader_dir);
        auto bloom = engine::renderer::BloomPass::create(device, allocator, *pipeline_cache,
                                                         shader_dir);
        auto expo = engine::renderer::ExposurePass::create(device, allocator, *pipeline_cache,
                                                           shader_dir);
        auto taa = engine::renderer::TaaPass::create(device, allocator, *pipeline_cache, shader_dir,
                                                     ldr_format);
#ifdef ENGINE_EDITOR
        if (editor.active()) {
            auto ui = engine::renderer::ImGuiPass::create(device, allocator, *pipeline_cache,
                                                          setup_transfer, shader_dir,
                                                          swapchain.format());
            if (!ui) {
                frames_ok = false;
                std::fprintf(stderr, "[fatal] imgui pass creation failed: %s\n",
                             ui.error().message.c_str());
                break;
            }
            f.imgui = std::move(*ui);
        }
#endif
        if (!mesh || !light || !tone || !skin || !shadow || !transp || !bloom || !expo || !taa) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] deferred pass creation failed: %s\n",
                         (!mesh    ? mesh.error()
                          : !light  ? light.error()
                          : !tone   ? tone.error()
                          : !skin   ? skin.error()
                          : !shadow ? shadow.error()
                          : !transp ? transp.error()
                          : !bloom  ? bloom.error()
                          : !expo   ? expo.error()
                                    : taa.error())
                             .message.c_str());
            break;
        }
        f.mesh = std::move(*mesh);
        f.lighting = std::move(*light);
        f.tonemap = std::move(*tone);
        f.skinning = std::move(*skin);
        f.shadow = std::move(*shadow);
        f.transparent = std::move(*transp);
        f.bloom = std::move(*bloom);
        f.exposure = std::move(*expo);
        f.taa = std::move(*taa);
        f.pool = engine::rhi::TransientImagePool(device, allocator);
    }

    // Scene assets live in an AssetManager so MeshRenderer handles keep them
    // alive. QUAT_GLTF=<path> instantiates a real glTF scene graph; otherwise a
    // spinning demo cube stands in.
    engine::asset::AssetManager assets;
    engine::scene::Scene scene;
    entt::entity spin_entity = entt::null; // the demo cube / glass sphere spins
    glm::vec3 spin_pos{0.0F};               // preserved across the per-frame spin
    float spin_scale = 1.0F;
    std::optional<engine::physics::PhysicsWorld> physics_world; // set in physics/character demo
    bool physics_mode = false;
    bool character_mode = false;
    entt::entity character_entity = entt::null;
    bool scene_ok = true;

    if (const char* gltf_path = std::getenv("QUAT_GLTF"); gltf_path != nullptr) {
        auto roots = engine::scene::GltfLoader::instantiate(gltf_path, allocator, setup_transfer,
                                                            assets, scene);
        if (!roots) {
            std::fprintf(stderr, "[fatal] glTF instantiate failed: %s\n",
                         roots.error().message.c_str());
            scene_ok = false;
        } else {
            std::fprintf(stderr, "[info] loaded '%s': %zu root node(s), %zu entities\n", gltf_path,
                         roots->size(),
                         static_cast<std::size_t>(
                             scene.registry().view<engine::scene::Transform>().size()));
        }
    } else if (std::getenv("QUAT_PHYSICS") != nullptr) {
        // Physics demo: a pile of boxes dropped onto a static floor.
        physics_mode = true;
        using engine::scene::BodyMotion;
        using engine::scene::Collider;
        using engine::scene::ColliderShape;
        using engine::scene::MeshRenderer;
        using engine::scene::RigidBody;
        using engine::scene::Transform;
        constexpr std::uint32_t no_body = engine::physics::PhysicsWorld::invalid_body;

        auto world = engine::physics::PhysicsWorld::create();
        auto floor_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/floor", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(10.0F, 0.5F, 10.0F)),
                                                  allocator, setup_transfer);
            });
        auto box_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/box", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(0.5F)), allocator,
                                                  setup_transfer);
            });
        auto floor_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/floor_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.5F, 0.5F, 0.55F, 1.0F};
                p.metallic_factor = 0.0F;
                p.roughness_factor = 0.9F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });
        auto box_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/box_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.85F, 0.4F, 0.2F, 1.0F};
                p.metallic_factor = 0.0F;
                p.roughness_factor = 0.5F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });

        if (!world || !floor_mesh.is_loaded() || !box_mesh.is_loaded()) {
            scene_ok = false;
        } else {
            physics_world = std::move(*world);

            const entt::entity floor = scene.create_entity("floor");
            scene.registry().get<Transform>(floor).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -0.5F, 0.0F));
            scene.registry().emplace<MeshRenderer>(floor, floor_mesh, floor_mat);
            scene.registry().emplace<Collider>(floor, ColliderShape::box,
                                               glm::vec3(10.0F, 0.5F, 10.0F), glm::vec3(0.0F));
            scene.registry().emplace<RigidBody>(floor, BodyMotion::static_body, 1.0F, no_body);

            // A stack of boxes, jittered into 3x3 columns climbing upward.
            for (int i = 0; i < 12; ++i) {
                const float fx = static_cast<float>((i % 3) - 1) * 0.6F;
                const float fz = static_cast<float>((i / 3) % 3 - 1) * 0.6F;
                const float fy = 3.0F + static_cast<float>(i) * 1.15F;
                const entt::entity b = scene.create_entity("box");
                scene.registry().get<Transform>(b).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(fx, fy, fz)) *
                    glm::rotate(glm::mat4(1.0F), 0.3F * static_cast<float>(i),
                                glm::normalize(glm::vec3(1.0F, 0.6F, 0.3F)));
                scene.registry().emplace<MeshRenderer>(b, box_mesh, box_mat);
                scene.registry().emplace<Collider>(b, ColliderShape::box, glm::vec3(0.5F),
                                                   glm::vec3(0.0F));
                scene.registry().emplace<RigidBody>(b, BodyMotion::dynamic, 1.0F, no_body);
            }

            // A transparent glass panel in front of the pile (visual only).
            auto glass_mesh = assets.load<engine::asset::MeshAsset>(
                "demo/glass", [&](const std::filesystem::path&) {
                    return engine::scene::upload_mesh(make_box_data(glm::vec3(3.0F, 2.2F, 0.08F)),
                                                      allocator, setup_transfer);
                });
            auto glass_mat = assets.load<engine::asset::MaterialAsset>(
                "demo/glass_mat", [&](const std::filesystem::path&) {
                    engine::asset::PbrMaterialParams p;
                    p.base_color_factor = {0.75F, 0.88F, 1.0F, 1.0F}; // light blue tint
                    p.transmission_factor = 1.0F;
                    p.metallic_factor = 0.0F;
                    p.roughness_factor = 0.05F; // clear glass
                    p.flags = engine::asset::material_transmission;
                    return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                          allocator, setup_transfer);
                });
            if (glass_mesh.is_loaded()) {
                const entt::entity glass = scene.create_entity("glass");
                scene.registry().get<Transform>(glass).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.2F, 3.0F));
                scene.registry().emplace<MeshRenderer>(glass, glass_mesh, glass_mat);
            }
        }
    } else if (std::getenv("QUAT_CHARACTER") != nullptr) {
        // Character demo: walk a capsule around a floor with WASD (third-person).
        character_mode = true;
        using engine::scene::Camera;
        using engine::scene::CharacterController;
        using engine::scene::MeshRenderer;
        using engine::scene::Transform;

        auto world = engine::physics::PhysicsWorld::create();
        auto floor_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/cfloor", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(10.0F, 0.5F, 10.0F)),
                                                  allocator, setup_transfer);
            });
        // Capsule stand-in: a box sized to the capsule's bounding volume.
        auto body_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/player", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(0.3F, 0.9F, 0.3F)),
                                                  allocator, setup_transfer);
            });
        auto floor_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/cfloor_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.5F, 0.5F, 0.55F, 1.0F};
                p.roughness_factor = 0.9F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });
        auto body_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/player_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.25F, 0.5F, 0.85F, 1.0F};
                p.roughness_factor = 0.4F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });

        if (!world || !floor_mesh.is_loaded() || !body_mesh.is_loaded()) {
            scene_ok = false;
        } else {
            // Static floor body (collision) added directly to the world.
            engine::physics::PhysicsWorld::BodyParams floor_body;
            floor_body.shape = world->create_box(glm::vec3(10.0F, 0.5F, 10.0F));
            floor_body.position = glm::vec3(0.0F, -0.5F, 0.0F);
            floor_body.motion = engine::physics::Motion::static_body;
            floor_body.layer = engine::physics::Layer::static_body;
            (void)world->add_body(floor_body);
            physics_world = std::move(*world);

            const entt::entity floor = scene.create_entity("floor");
            scene.registry().get<Transform>(floor).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -0.5F, 0.0F));
            scene.registry().emplace<MeshRenderer>(floor, floor_mesh, floor_mat);

            character_entity = scene.create_entity("player");
            scene.registry().get<Transform>(character_entity).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 0.0F));
            scene.registry().emplace<MeshRenderer>(character_entity, body_mesh, body_mat);
            scene.registry().emplace<CharacterController>(character_entity);
        }
    } else {
        // Showcase scene: a ground plane under the sky, a row of PBR spheres
        // sweeping roughness (top, metallic) and another sweeping metalness
        // (bottom, dielectric), a warm emissive cube, and a clear-glass sphere
        // that refracts the scene behind it. Sun + sky + point lights added below.
        using engine::scene::MeshRenderer;
        using engine::scene::Transform;
        auto make_mat = [&](const std::string& id, glm::vec4 color, float metal, float rough,
                            glm::vec3 emissive = glm::vec3(0.0F), std::uint32_t flags = 0) {
            return assets.load<engine::asset::MaterialAsset>(
                id, [&](const std::filesystem::path&) {
                    engine::asset::PbrMaterialParams p;
                    p.base_color_factor = color;
                    p.metallic_factor = metal;
                    p.roughness_factor = rough;
                    p.emissive_factor = glm::vec4(emissive, 0.0F);
                    if ((flags & engine::asset::material_transmission) != 0) {
                        p.transmission_factor = 1.0F;
                    }
                    p.flags = flags;
                    return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                          allocator, setup_transfer);
                });
        };

        auto ground_mesh = assets.load<engine::asset::MeshAsset>(
            "show/ground", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(14.0F, 0.25F, 14.0F)),
                                                  allocator, setup_transfer);
            });
        auto sphere_mesh = assets.load<engine::asset::MeshAsset>(
            "show/sphere", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_sphere_data(0.55F, 32, 48), allocator,
                                                  setup_transfer);
            });
        auto cube_mesh = assets.load<engine::asset::MeshAsset>(
            "show/cube", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(0.5F)), allocator,
                                                  setup_transfer);
            });
        auto ground_mat = make_mat("show/ground_mat", {0.42F, 0.45F, 0.5F, 1.0F}, 0.0F, 0.85F);

        if (!ground_mesh.is_loaded() || !sphere_mesh.is_loaded() || !cube_mesh.is_loaded()) {
            scene_ok = false;
        } else {
            const entt::entity ground = scene.create_entity("ground");
            scene.registry().get<Transform>(ground).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -0.25F, 0.0F));
            scene.registry().emplace<MeshRenderer>(ground, ground_mesh, ground_mat);

            constexpr int count = 6;
            for (int i = 0; i < count; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                const float x = (t - 0.5F) * 7.5F;
                // Top row: polished metal, roughness sweep.
                const entt::entity m = scene.create_entity("metal_sphere");
                scene.registry().get<Transform>(m).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.55F, -1.6F));
                scene.registry().emplace<MeshRenderer>(
                    m, sphere_mesh,
                    make_mat("show/metal" + std::to_string(i), {0.95F, 0.78F, 0.45F, 1.0F}, 1.0F,
                             glm::clamp(t, 0.05F, 0.95F)));
                // Bottom row: coloured dielectric, metalness sweep.
                const entt::entity d_ent = scene.create_entity("dielectric_sphere");
                scene.registry().get<Transform>(d_ent).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.55F, 1.4F));
                scene.registry().emplace<MeshRenderer>(
                    d_ent, sphere_mesh,
                    make_mat("show/diel" + std::to_string(i), {0.2F, 0.45F, 0.85F, 1.0F},
                             glm::clamp(t, 0.0F, 1.0F), 0.35F));
            }

            // A warm emissive cube as a visible local light source.
            const entt::entity glow = scene.create_entity("glow_cube");
            scene.registry().get<Transform>(glow).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(-5.5F, 0.5F, 4.0F)) *
                glm::rotate(glm::mat4(1.0F), 0.6F, glm::vec3(0.2F, 1.0F, 0.1F));
            scene.registry().emplace<MeshRenderer>(
                glow, cube_mesh,
                make_mat("show/glow_mat", {0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, 1.0F,
                         glm::vec3(3.0F, 1.2F, 0.3F)));

            // A clear glass sphere up front that refracts the spheres behind it.
            const entt::entity glass = scene.create_entity("glass_sphere");
            spin_entity = glass; // gently rotate it so the refraction shifts
            spin_pos = glm::vec3(0.0F, 0.85F, 4.2F);
            spin_scale = 1.5F;
            scene.registry().get<Transform>(glass).local =
                glm::translate(glm::mat4(1.0F), spin_pos) *
                glm::scale(glm::mat4(1.0F), glm::vec3(spin_scale));
            scene.registry().emplace<MeshRenderer>(
                glass, sphere_mesh,
                make_mat("show/glass_mat", {0.85F, 0.92F, 1.0F, 1.0F}, 0.0F, 0.04F,
                         glm::vec3(0.0F), engine::asset::material_transmission));
        }
    }

    vkDestroyCommandPool(vk_device, setup_pool, nullptr);

    if (!frames_ok || !scene_ok) {
        fatal("deferred renderer setup failed", "see [fatal] lines above");
        return 1;
    }

    // A sun light, and a camera that auto-frames whatever was loaded.
    const entt::entity sun_entity = scene.create_entity("sun");
    scene.registry().emplace<engine::scene::DirectionalLight>(
        sun_entity, glm::normalize(glm::vec3(-0.4F, -1.0F, -0.3F)),
        glm::vec3(1.0F, 0.98F, 0.95F), 3.0F);

    // Two coloured point lights for warmer/cooler fill (skipped for bare glTF
    // loads so a model is shown under just its own lighting setup).
    if (!std::getenv("QUAT_GLTF")) {
        const entt::entity warm = scene.create_entity("point_warm");
        scene.registry().get<engine::scene::Transform>(warm).local =
            glm::translate(glm::mat4(1.0F), glm::vec3(-3.5F, 3.0F, 2.5F));
        scene.registry().emplace<engine::scene::PointLight>(warm, glm::vec3(1.0F, 0.55F, 0.2F),
                                                            14.0F, 12.0F);
        const entt::entity cool = scene.create_entity("point_cool");
        scene.registry().get<engine::scene::Transform>(cool).local =
            glm::translate(glm::mat4(1.0F), glm::vec3(3.5F, 3.0F, -2.5F));
        scene.registry().emplace<engine::scene::PointLight>(cool, glm::vec3(0.3F, 0.5F, 1.0F),
                                                            14.0F, 12.0F);
    }

    // Image-based lighting: bake the environment (procedural sky) into a diffuse
    // irradiance cube + prefiltered specular cube + BRDF LUT once, using the sun
    // direction. The deferred lighting pass samples these every frame for its
    // ambient/reflection term, so metals and smooth surfaces reflect the sky
    // instead of reading as flat grey. Falls back to flat ambient on failure.
    engine::renderer::IblMaps ibl_maps;
    {
        glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 1.0F, 0.3F));
        const auto dlv = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : dlv) {
            sun_to = -glm::normalize(dlv.get<const engine::scene::DirectionalLight>(e).direction);
            break;
        }
        if (auto baker = engine::renderer::IblBaker::create(device, allocator, *pipeline_cache,
                                                            shader_dir);
            baker) {
            if (auto baked = baker->bake(sun_to); baked) {
                ibl_maps = std::move(*baked);
                std::fprintf(stderr, "[info] baked IBL environment\n");
            } else {
                std::fprintf(stderr, "[warn] IBL bake failed: %s\n", baked.error().message.c_str());
            }
        } else {
            std::fprintf(stderr, "[warn] IBL baker create failed: %s\n",
                         baker.error().message.c_str());
        }
    }

    const entt::entity camera_entity = scene.create_entity("camera");
    const engine::scene::Camera& camera =
        scene.registry().emplace<engine::scene::Camera>(camera_entity);
    scene.tick(); // resolve world transforms before measuring the scene bounds
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    const bool has_geo = scene_world_bounds(scene, bmin, bmax);
    const glm::mat4 framed =
        has_geo ? frame_camera_world(bmin, bmax, camera.fov_y)
                : glm::inverse(glm::lookAt(glm::vec3(3.0F, 2.5F, 4.0F), glm::vec3(0.0F),
                                           glm::vec3(0.0F, 1.0F, 0.0F)));

    // Free-fly camera state, seeded from the framed transform so the opening view
    // matches the auto-frame. Updated each frame from input (WASD + right-drag).
    // The physics demo's huge floor would push the auto-frame far away; use a
    // fixed view of the drop zone instead.
    const glm::mat4 framed_view =
        physics_mode ? glm::inverse(glm::lookAt(glm::vec3(8.0F, 5.0F, 10.0F),
                                                glm::vec3(0.0F, 1.5F, 0.0F),
                                                glm::vec3(0.0F, 1.0F, 0.0F)))
                     : framed;
    glm::vec3 cam_pos = glm::vec3(framed_view[3]);
    const glm::vec3 cam_fwd0 = -glm::normalize(glm::vec3(framed_view[2])); // camera looks down -Z
    float cam_yaw = std::atan2(cam_fwd0.z, cam_fwd0.x);
    float cam_pitch = std::asin(glm::clamp(cam_fwd0.y, -0.999F, 0.999F));
    const float move_speed = physics_mode ? 6.0F
                             : has_geo    ? glm::max(1.0F, 0.5F * glm::length(bmax - bmin))
                                          : 3.0F;

    // ---- Per-frame command + sync resources --------------------------------
    VkCommandPool command_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(vk_device, &pool_info, nullptr, &command_pool);
    }

    std::array<VkCommandBuffer, k_frames_in_flight> command_buffers{};
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = command_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = k_frames_in_flight;
        vkAllocateCommandBuffers(vk_device, &alloc, command_buffers.data());
    }

    auto make_semaphore = [vk_device] {
        VkSemaphore s = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(vk_device, &info, nullptr, &s);
        return s;
    };

    // image_available + in-flight fence are per frame-in-flight; render_finished
    // is per swapchain image (it is signalled by the submit that targets that
    // image and waited on by its present).
    std::array<VkSemaphore, k_frames_in_flight> image_available{};
    std::array<VkFence, k_frames_in_flight> in_flight{};
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        image_available[i] = make_semaphore();
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk_device, &fence_info, nullptr, &in_flight[i]);
    }

    std::vector<VkSemaphore> render_finished(swapchain.image_count(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_finished) {
        s = make_semaphore();
    }

    auto recreate_render_finished = [&] {
        for (VkSemaphore s : render_finished) {
            vkDestroySemaphore(vk_device, s, nullptr);
        }
        render_finished.assign(swapchain.image_count(), VK_NULL_HANDLE);
        for (VkSemaphore& s : render_finished) {
            s = make_semaphore();
        }
    };

    // ---- Main loop ---------------------------------------------------------
    // QUAT_MAX_FRAMES, when set, auto-quits after that many presented frames.
    // Used for headless smoke tests so clean teardown can be verified.
    std::uint64_t max_frames = 0;
    if (const char* env = std::getenv("QUAT_MAX_FRAMES"); env != nullptr) {
        max_frames = std::strtoull(env, nullptr, 10);
    }
    // QUAT_SCREENSHOT, combined with QUAT_MAX_FRAMES, writes the final presented
    // frame to the given path as binary PPM — headless visual verification.
    const char* screenshot_path = std::getenv("QUAT_SCREENSHOT");
    engine::rhi::GpuBuffer screenshot_buffer;
    bool screenshot_recorded = false;

    std::uint64_t presented = 0;
    std::uint32_t frame = 0;
    bool running = true;
    bool needs_recreate = false;
    bool looking = false; // right mouse button held -> mouse-look active
    std::uint64_t last_ticks = SDL_GetTicksNS();
    glm::mat4 prev_jittered_vp(1.0F); // previous frame's jittered view-proj, for TAA
    bool taa_history_valid = false;

    while (running) {
        // Per-frame accumulated mouse-look delta.
        float look_dx = 0.0F;
        float look_dy = 0.0F;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
#ifdef ENGINE_EDITOR
            editor.process_event(event);
#endif
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                needs_recreate = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
#ifdef ENGINE_EDITOR
                // Camera input belongs to the Viewport panel; clicks anywhere
                // else are the UI's.
                if (editor.active() && !editor.viewport_hovered()) break;
#endif
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    looking = true;
                    SDL_SetWindowRelativeMouseMode(window, true);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    looking = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (looking) {
                    look_dx += event.motion.xrel;
                    look_dy += event.motion.yrel;
                }
                break;
            default:
                break;
            }
        }
        if (!running) break;

#ifdef ENGINE_EDITOR
        editor.begin_frame();

        // IBL rebake requested from the Renderer panel (sun edited via its
        // entity). bake() waits the graphics queue idle internally before
        // returning, so replacing the maps cannot race in-flight frames.
        if (render_settings.rebake_ibl) {
            render_settings.rebake_ibl = false;
            glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 1.0F, 0.3F));
            const auto dlv = scene.registry().view<const engine::scene::DirectionalLight>();
            for (const entt::entity e : dlv) {
                sun_to =
                    -glm::normalize(dlv.get<const engine::scene::DirectionalLight>(e).direction);
                break;
            }
            if (auto baker = engine::renderer::IblBaker::create(device, allocator,
                                                                *pipeline_cache, shader_dir);
                baker) {
                if (auto baked = baker->bake(sun_to); baked) {
                    ibl_maps = std::move(*baked);
                    std::fprintf(stderr, "[info] rebaked IBL environment\n");
                }
            }
        }

        // glTF instantiate requested from the asset browser (double-click or
        // viewport drop): blocking load through a transient upload context on
        // the graphics queue (mip blits), like the startup path.
        if (!pending_instantiate.empty()) {
            const std::string path = std::exchange(pending_instantiate, {});
            VkCommandPool load_pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = device.queue_families().graphics;
            if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &load_pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext load_transfer{
                    vk_device, load_pool, device.graphics_queue(),
                    device.max_sampler_anisotropy()};
                if (auto roots = engine::scene::GltfLoader::instantiate(
                        path, allocator, load_transfer, assets, scene);
                    roots) {
                    std::fprintf(stderr, "[editor] instantiated %s (%zu roots)\n", path.c_str(),
                                 roots->size());
                } else {
                    std::fprintf(stderr, "[editor] instantiate failed: %s\n",
                                 roots.error().message.c_str());
                }
                vkDestroyCommandPool(vk_device, load_pool, nullptr);
            }
        }
#endif

        const VkExtent2D pixel_extent = window_pixel_extent(window);
        if (pixel_extent.width == 0 || pixel_extent.height == 0) {
            SDL_Delay(10); // minimized — nothing to draw
            continue;
        }

        if (needs_recreate) {
            if (auto r = swapchain.recreate(pixel_extent); !r) {
                std::fprintf(stderr, "[fatal] swapchain recreate failed: %s\n",
                             r.error().message.c_str());
                break;
            }
            recreate_render_finished();
            needs_recreate = false;
        }

        vkWaitForFences(vk_device, 1, &in_flight[frame], VK_TRUE, UINT64_MAX);

        std::uint32_t image_index = 0;
        VkResult acquire = vkAcquireNextImageKHR(
            vk_device, swapchain.handle(), UINT64_MAX, image_available[frame],
            VK_NULL_HANDLE, &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            needs_recreate = true;
            continue;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            std::fprintf(stderr, "[fatal] vkAcquireNextImageKHR failed (%d)\n",
                         static_cast<int>(acquire));
            break;
        }

        vkResetFences(vk_device, 1, &in_flight[frame]);

        VkCommandBuffer cmd = command_buffers[frame];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        // Build this frame's deferred graph into the slot's own resources and
        // render the scene to the acquired swapchain image. The slot's fence was
        // waited above, so resetting its transient pool and rebuilding the passes'
        // descriptor buffers does not race the other in-flight frame.
        DeferredFrame& fr = frames[frame];
        fr.pool.reset();

        // The scene renders at `draw_extent`; the swapchain presents at
        // `present_extent`. They differ only in editor mode, where the scene
        // fills the Viewport panel (sized by last frame's UI) instead of the
        // whole window.
        const VkExtent2D present_extent = swapchain.extent();
        VkExtent2D draw_extent = present_extent;
#ifdef ENGINE_EDITOR
        if (editor.active() && editor.viewport_width() > 0 && editor.viewport_height() > 0) {
            draw_extent.width = std::clamp(editor.viewport_width(), 16U, 4096U);
            draw_extent.height = std::clamp(editor.viewport_height(), 16U, 4096U);
        }
#endif
        const float aspect =
            draw_extent.height == 0
                ? 1.0F
                : static_cast<float>(draw_extent.width) / static_cast<float>(draw_extent.height);

        // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift = fast.
        const std::uint64_t now_ticks = SDL_GetTicksNS();
        const float dt = static_cast<float>(now_ticks - last_ticks) * 1e-9F;
        last_ticks = now_ticks;

        constexpr float look_sensitivity = 0.0025F;
        cam_yaw += look_dx * look_sensitivity;
        cam_pitch = glm::clamp(cam_pitch - look_dy * look_sensitivity, glm::radians(-89.0F),
                               glm::radians(89.0F));
        const glm::vec3 world_up(0.0F, 1.0F, 0.0F);
        const bool* keys = SDL_GetKeyboardState(nullptr);
#ifdef ENGINE_EDITOR
        // A focused UI text field owns the keyboard — freeze game movement.
        const bool game_keys = !editor.wants_keyboard();
#else
        const bool game_keys = true;
#endif
        const auto axis = [&](SDL_Scancode pos, SDL_Scancode neg) {
            if (!game_keys) return 0.0F;
            return (keys[pos] ? 1.0F : 0.0F) - (keys[neg] ? 1.0F : 0.0F);
        };

        if (character_mode) {
            // WASD drives the character along the camera's heading (third-person).
            const glm::vec3 fwd_h =
                glm::normalize(glm::vec3(std::cos(cam_yaw), 0.0F, std::sin(cam_yaw)));
            const glm::vec3 right_h = glm::normalize(glm::cross(fwd_h, world_up));
            const glm::vec3 wish = fwd_h * axis(SDL_SCANCODE_W, SDL_SCANCODE_S) +
                                   right_h * axis(SDL_SCANCODE_D, SDL_SCANCODE_A);
            scene.registry().get<engine::scene::CharacterController>(character_entity).move =
                wish != glm::vec3(0.0F) ? glm::normalize(wish) : glm::vec3(0.0F);
        } else {
            // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift fast.
            const glm::vec3 cam_fwd(std::cos(cam_pitch) * std::cos(cam_yaw), std::sin(cam_pitch),
                                    std::cos(cam_pitch) * std::sin(cam_yaw));
            const glm::vec3 cam_right = glm::normalize(glm::cross(cam_fwd, world_up));
            const glm::vec3 move = cam_fwd * axis(SDL_SCANCODE_W, SDL_SCANCODE_S) +
                                   cam_right * axis(SDL_SCANCODE_D, SDL_SCANCODE_A) +
                                   world_up * axis(SDL_SCANCODE_E, SDL_SCANCODE_Q);
            const float speed = move_speed * (keys[SDL_SCANCODE_LSHIFT] ? 4.0F : 1.0F);
            if (move != glm::vec3(0.0F)) {
                cam_pos += glm::normalize(move) * speed * dt;
            }
            scene.registry().get<engine::scene::Transform>(camera_entity).local =
                glm::inverse(glm::lookAt(cam_pos, cam_pos + cam_fwd, world_up));
        }

        // Spin the demo cube (if present), then step physics / the character.
        if (spin_entity != entt::null) {
            scene.registry().get<engine::scene::Transform>(spin_entity).local =
                glm::translate(glm::mat4(1.0F), spin_pos) *
                glm::rotate(glm::mat4(1.0F), static_cast<float>(presented) * 0.02F,
                            glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F))) *
                glm::scale(glm::mat4(1.0F), glm::vec3(spin_scale));
        }
        if (physics_world) {
            if (character_mode) {
                engine::scene::character_system(scene.registry(), *physics_world, dt);
            } else {
                engine::scene::physics_system(scene.registry(), *physics_world, dt);
            }
        }
        scene.tick(dt); // advances any Animators by real elapsed time

        // Third-person follow camera, placed after the character has moved.
        if (character_mode) {
            const glm::vec3 target =
                glm::vec3(scene.registry()
                              .get<engine::scene::Transform>(character_entity)
                              .world[3]) +
                glm::vec3(0.0F, 0.8F, 0.0F);
            const glm::vec3 cam_fwd(std::cos(cam_pitch) * std::cos(cam_yaw), std::sin(cam_pitch),
                                    std::cos(cam_pitch) * std::sin(cam_yaw));
            cam_pos = target - cam_fwd * 5.0F;
            const glm::mat4 view_inv = glm::inverse(glm::lookAt(cam_pos, target, world_up));
            auto& camera_tf = scene.registry().get<engine::scene::Transform>(camera_entity);
            camera_tf.local = view_inv;
            camera_tf.world = view_inv;
        }
        const engine::scene::CameraMatrices cam =
            engine::scene::camera_system(scene.registry(), aspect);

        // TAA: jitter the projection sub-pixel each frame (Halton). The whole
        // frame renders with the jittered view-proj; the resolve reprojects and
        // accumulates, and prev_jittered_vp drives the reprojection.
        const glm::vec2 jitter =
            engine::renderer::taa_jitter(presented, draw_extent.width, draw_extent.height);
        glm::mat4 jitter_mat(1.0F);
        jitter_mat[3][0] = jitter.x;
        jitter_mat[3][1] = jitter.y;
        const glm::mat4 jittered_vp = jitter_mat * cam.view_proj;
        const glm::mat4 jittered_inv_vp = glm::inverse(jittered_vp);

        engine::renderer::DirectionalLightParams light;
        const auto light_view = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : light_view) {
            const auto& dl = light_view.get<const engine::scene::DirectionalLight>(e);
            light.direction = glm::vec4(-glm::normalize(dl.direction), 0.0F);
            light.color = glm::vec4(dl.color, dl.intensity);
            break;
        }

        // Gather point lights from the ECS (world position from Transform).
        std::vector<engine::renderer::PointLightGpu> point_lights;
        for (auto [e, t, pl] :
             scene.registry()
                 .view<const engine::scene::Transform, const engine::scene::PointLight>()
                 .each()) {
            engine::renderer::PointLightGpu g;
            g.position_radius = glm::vec4(glm::vec3(t.world[3]), pl.radius);
            g.color_intensity = glm::vec4(pl.color, pl.intensity);
            point_lights.push_back(g);
        }

        // Build the draw list, collecting per-skinned-entity skinning work. A
        // skinned entity uploads its joint matrices into this frame slot's buffer
        // and gets a SkinDispatch; its draw points at the skinned output buffer.
        std::vector<engine::renderer::DrawItem> draws;
        std::vector<engine::renderer::DrawItem> transparent_draws;
        std::vector<engine::renderer::SkinDispatch> dispatches;
        for (auto [e, t, mr] :
             scene.registry().view<engine::scene::Transform, engine::scene::MeshRenderer>().each()) {
            engine::renderer::DrawItem item;
            item.mesh = mr.mesh.valid() ? &*mr.mesh : nullptr;
            item.material = mr.material.valid() ? &*mr.material : nullptr;
            item.model = t.world;

            const auto* sm = scene.registry().try_get<engine::scene::SkinnedMesh>(e);
            if (item.mesh != nullptr && item.mesh->skinned() && sm != nullptr &&
                !sm->joint_matrices.empty()) {
                SkinnedEntityGpu* g = ensure_skin_gpu(
                    fr, e, *item.mesh, vk_device, allocator,
                    static_cast<std::uint32_t>(sm->joint_matrices.size()));
                if (g != nullptr) {
                    std::memcpy(g->joints.mapped(), sm->joint_matrices.data(),
                                sm->joint_matrices.size() * sizeof(glm::mat4));
                    engine::renderer::SkinDispatch d;
                    d.vertices_in =
                        engine::rhi::buffer_device_address(vk_device, item.mesh->vertex_buffer);
                    d.vertices_in_size = static_cast<VkDeviceSize>(item.mesh->vertex_count) *
                                         sizeof(engine::asset::Vertex);
                    d.skin = engine::rhi::buffer_device_address(vk_device, item.mesh->skin_buffer);
                    d.skin_size = static_cast<VkDeviceSize>(item.mesh->vertex_count) *
                                  sizeof(engine::asset::SkinVertex);
                    d.joints = g->joints_addr;
                    d.joints_size = static_cast<VkDeviceSize>(g->joint_count) * sizeof(glm::mat4);
                    d.vertices_out = g->skinned_addr;
                    d.vertices_out_size = static_cast<VkDeviceSize>(g->vertex_count) *
                                          sizeof(engine::asset::Vertex);
                    d.vertex_count = item.mesh->vertex_count;
                    dispatches.push_back(d);
                    item.skinned_vertices = g->skinned_verts.handle();
                    // Skinned vertices are already in skin space (glTF ignores the
                    // mesh node transform), so draw them with an identity model.
                    item.model = glm::mat4(1.0F);
                }
            }
            // Route alpha-blended + transmissive (glass) materials to the forward pass.
            const bool is_transparent =
                item.material != nullptr &&
                (item.material->params.flags &
                 (engine::asset::material_blend | engine::asset::material_transmission)) != 0;
            (is_transparent ? transparent_draws : draws).push_back(item);
        }

        // Sort transparent draws back-to-front (farthest from the camera first).
        std::sort(transparent_draws.begin(), transparent_draws.end(),
                  [&](const engine::renderer::DrawItem& a, const engine::renderer::DrawItem& b) {
                      const float da = glm::distance(glm::vec3(a.model[3]), cam.position);
                      const float db = glm::distance(glm::vec3(b.model[3]), cam.position);
                      return da > db;
                  });

        // Shadow casters: all opaques plus transmissive glass (solid surfaces —
        // they block the sun like opaque geometry; alpha-blended items do not cast).
        std::vector<engine::renderer::DrawItem> shadow_draws = draws;
        for (const engine::renderer::DrawItem& d : transparent_draws) {
            if (d.material != nullptr &&
                (d.material->params.flags & engine::asset::material_transmission) != 0) {
                shadow_draws.push_back(d);
            }
        }

        // Directional shadow: an orthographic light view fit to a fixed box
        // around the scene (the demos all sit near the origin).
        const glm::vec3 sun_dir = -glm::normalize(glm::vec3(light.direction)); // travel direction
        const glm::vec3 shadow_center(0.0F, 1.5F, 0.0F);
        constexpr float shadow_radius = 14.0F;
        const glm::vec3 light_eye = shadow_center - sun_dir * (shadow_radius * 1.5F);
        const glm::mat4 shadow_view =
            glm::lookAt(light_eye, shadow_center, glm::vec3(0.0F, 1.0F, 0.0F));
        const glm::mat4 shadow_proj = glm::ortho(-shadow_radius, shadow_radius, -shadow_radius,
                                                 shadow_radius, 0.1F, shadow_radius * 3.5F);
        const glm::mat4 light_view_proj = shadow_proj * shadow_view;

        engine::rhi::RenderGraph graph(fr.pool);
        auto shadow = fr.shadow.add_to_graph(graph, light_view_proj, shadow_draws);
        if (!shadow) {
            std::fprintf(stderr, "[fatal] shadow pass: %s\n", shadow.error().message.c_str());
            break;
        }
        auto gbuffer = fr.mesh.add_to_graph(graph, draw_extent, jittered_vp, draws);
        if (!gbuffer) {
            std::fprintf(stderr, "[fatal] gbuffer pass: %s\n", gbuffer.error().message.c_str());
            break;
        }
        auto hdr = fr.lighting.add_to_graph(graph, *gbuffer, draw_extent, light, jittered_inv_vp,
                                            cam.position, *shadow, light_view_proj, point_lights,
                                            /*enable_sky=*/true, &ibl_maps);
        if (!hdr) {
            std::fprintf(stderr, "[fatal] lighting pass: %s\n", hdr.error().message.c_str());
            break;
        }
        // Forward-blend transparent geometry over the lit HDR, depth-tested
        // against the opaque GBuffer depth.
        if (!transparent_draws.empty()) {
            if (auto r = fr.transparent.add_to_graph(graph, *hdr, gbuffer->depth, draw_extent,
                                                     jittered_vp, light, cam.position,
                                                     transparent_draws, &ibl_maps);
                !r) {
                std::fprintf(stderr, "[fatal] transparent pass: %s\n", r.error().message.c_str());
                break;
            }
        }
        // Bloom: blur the bright parts of the HDR and add them back before tonemap.
        engine::renderer::BloomParams bloom_params{};
#ifdef ENGINE_EDITOR
        bloom_params.threshold = render_settings.bloom_threshold;
        bloom_params.knee = render_settings.bloom_knee;
        bloom_params.intensity = render_settings.bloom_intensity;
        bloom_params.radius = render_settings.bloom_radius;
#endif
        if (auto r = fr.bloom.add_to_graph(graph, *hdr, draw_extent, bloom_params); !r) {
            std::fprintf(stderr, "[fatal] bloom pass: %s\n", r.error().message.c_str());
            break;
        }
        // Auto-exposure: measure the (post-bloom) HDR luminance and adapt over time.
        if (auto r = fr.exposure.add_to_graph(graph, *hdr, draw_extent, dt); !r) {
            std::fprintf(stderr, "[fatal] exposure pass: %s\n", r.error().message.c_str());
            break;
        }
        const engine::rhi::ResourceHandle backbuffer = graph.import_image(
            "swapchain", swapchain.images()[image_index], swapchain.image_views()[image_index],
            swapchain.format(), present_extent, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        // In editor mode the frame resolves into the slot's offscreen viewport
        // image (sampled by the Viewport panel); otherwise straight into the
        // swapchain as before.
        engine::rhi::ResourceHandle scene_target = backbuffer;
#ifdef ENGINE_EDITOR
        engine::rhi::ResourceHandle viewport_handle{};
        if (editor.active()) {
            if (!ensure_viewport_target(fr, device, allocator, swapchain.format(), draw_extent)) {
                std::fprintf(stderr, "[fatal] viewport target creation failed\n");
                break;
            }
            viewport_handle = graph.import_image(
                "viewport", fr.viewport_image.handle(), fr.viewport_view.handle(),
                swapchain.format(), draw_extent, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            scene_target = viewport_handle;
        }
#endif
        // Tonemap to an intermediate LDR image; TAA resolves it into the target.
        const engine::rhi::ResourceHandle ldr_current = graph.create_transient_image(
            "ldr_current", ldr_format, draw_extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (auto r = fr.tonemap.add_to_graph(graph, *hdr, ldr_current, draw_extent,
                                             fr.exposure.exposure_buffer_address());
            !r) {
            std::fprintf(stderr, "[fatal] tonemap pass: %s\n", r.error().message.c_str());
            break;
        }
        // Temporal anti-aliasing: resolve the jittered frame against history.
        if (auto r = fr.taa.add_to_graph(graph, ldr_current, gbuffer->depth, scene_target,
                                         draw_extent, jittered_inv_vp, prev_jittered_vp, presented,
                                         taa_history_valid);
            !r) {
            std::fprintf(stderr, "[fatal] taa pass: %s\n", r.error().message.c_str());
            break;
        }
        prev_jittered_vp = jittered_vp;
        taa_history_valid = true;
#ifdef ENGINE_EDITOR
        // Editor UI: build the dockspace + panels (the Viewport panel samples
        // the offscreen scene image), then draw the UI into the swapchain as
        // the last pass. A null/empty draw list (editor disabled) adds no pass.
        {
            engine::editor::EditorContext ui_ctx;
            ui_ctx.frame_ms = dt * 1000.0F;
            ui_ctx.draw_count = static_cast<int>(draws.size() + transparent_draws.size());
            ui_ctx.entity_count = static_cast<int>(
                scene.registry().view<engine::scene::Transform>().size());
            ui_ctx.scene = &scene;
            ui_ctx.renderer = &render_settings;
            ui_ctx.project_root = QUAT_PROJECT_ROOT;
            ui_ctx.instantiate_request = &pending_instantiate;
            ui_ctx.view_proj = cam.view_proj; // unjittered, for overlays
            editor.build_ui(ui_ctx);
            if (auto r = fr.imgui.add_to_graph(graph, backbuffer, present_extent,
                                               editor.end_frame(), viewport_handle,
                                               /*clear=*/editor.active());
                !r) {
                std::fprintf(stderr, "[fatal] imgui pass: %s\n", r.error().message.c_str());
                break;
            }
        }
#endif
        if (auto compiled = graph.compile(); !compiled) {
            std::fprintf(stderr, "[fatal] render graph compile failed: %s\n",
                         compiled.error().message.c_str());
            break;
        }
        // Skin (compute) before the graph; record() adds the compute -> vertex
        // fetch barrier so the GBuffer sees the skinned vertices.
        if (!dispatches.empty()) {
            if (auto rs = fr.skinning.record(cmd, dispatches); !rs) {
                std::fprintf(stderr, "[fatal] skinning pass: %s\n", rs.error().message.c_str());
                break;
            }
        }
        graph.execute(cmd);

        // Screenshot capture on the final frame: the graph leaves the swapchain
        // image in PRESENT_SRC — round-trip it through TRANSFER_SRC and copy to
        // a host-visible buffer, written out as PPM after the queue idles.
        screenshot_recorded = false;
        if (screenshot_path != nullptr && max_frames != 0 && presented + 1 >= max_frames) {
            const VkDeviceSize shot_bytes =
                static_cast<VkDeviceSize>(present_extent.width) * present_extent.height * 4;
            if (screenshot_buffer.handle() == VK_NULL_HANDLE) {
                auto buf = allocator.create_buffer(shot_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_AUTO,
                                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
                if (buf) screenshot_buffer = std::move(*buf);
            }
            if (screenshot_buffer.handle() != VK_NULL_HANDLE) {
                const VkImage shot_image = swapchain.images()[image_index];
                VkImageMemoryBarrier2 to_src{};
                to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                to_src.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_src.image = shot_image;
                to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &to_src;
                vkCmdPipelineBarrier2(cmd, &dep);

                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {present_extent.width, present_extent.height, 1};
                vkCmdCopyImageToBuffer(cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       screenshot_buffer.handle(), 1, &region);

                VkImageMemoryBarrier2 to_present = to_src;
                to_present.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                to_present.dstAccessMask = 0;
                to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                dep.pImageMemoryBarriers = &to_present;
                vkCmdPipelineBarrier2(cmd, &dep);
                screenshot_recorded = true;
            }
        }

        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = image_available[frame];
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = render_finished[image_index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_info.commandBuffer = cmd;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        vkQueueSubmit2(device.graphics_queue(), 1, &submit, in_flight[frame]);

        VkSwapchainKHR sc = swapchain.handle();
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &image_index;
        VkResult present_result = vkQueuePresentKHR(device.graphics_queue(), &present);
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
            needs_recreate = true;
        } else if (present_result != VK_SUCCESS) {
            std::fprintf(stderr, "[fatal] vkQueuePresentKHR failed (%d)\n",
                         static_cast<int>(present_result));
            break;
        }

        if (screenshot_recorded) {
            vkQueueWaitIdle(device.graphics_queue());
            const bool bgra = swapchain.format() == VK_FORMAT_B8G8R8A8_SRGB ||
                              swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM;
            if (FILE* f = std::fopen(screenshot_path, "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", present_extent.width, present_extent.height);
                const auto* px = static_cast<const std::uint8_t*>(screenshot_buffer.mapped());
                std::vector<std::uint8_t> row(static_cast<std::size_t>(present_extent.width) * 3);
                for (std::uint32_t y = 0; y < present_extent.height; ++y) {
                    for (std::uint32_t x = 0; x < present_extent.width; ++x) {
                        const std::uint8_t* p =
                            px + (static_cast<std::size_t>(y) * present_extent.width + x) * 4;
                        row[x * 3 + 0] = bgra ? p[2] : p[0];
                        row[x * 3 + 1] = p[1];
                        row[x * 3 + 2] = bgra ? p[0] : p[2];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
                std::fprintf(stderr, "[screenshot] wrote %s\n", screenshot_path);
            } else {
                std::fprintf(stderr, "[screenshot] could not open %s\n", screenshot_path);
            }
        }

        frame = (frame + 1) % k_frames_in_flight;
        if (max_frames != 0 && ++presented >= max_frames) {
            running = false;
        }
    }

    // ---- Teardown ----------------------------------------------------------
    vkDeviceWaitIdle(vk_device);
    for (VkSemaphore s : render_finished) {
        vkDestroySemaphore(vk_device, s, nullptr);
    }
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        vkDestroySemaphore(vk_device, image_available[i], nullptr);
        vkDestroyFence(vk_device, in_flight[i], nullptr);
    }
    vkDestroyCommandPool(vk_device, command_pool, nullptr);
    swapchain = engine::rhi::Swapchain{};     // destroy swapchain before surface
    vkDestroySurfaceKHR(device.instance(), surface, nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
