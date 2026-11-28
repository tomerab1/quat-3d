#include "engine/renderer/star_field_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/photometry.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"star field: " + std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkBufferUsageFlags kStorage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VmaAllocationCreateFlags kHostWrite =
    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

// star_field.slang Star.
struct StarGpu {
    glm::vec4 direction_lux;
    glm::vec4 color_seed;
};
static_assert(sizeof(StarGpu) == 32);
constexpr VkDeviceSize kVisibleBytes = 32; // star_field.slang VisibleStar

// star_field.slang StarFrame (std140).
struct StarFrameGpu {
    glm::vec4 view_proj[4];
    glm::vec4 world_from_celestial[3];
    glm::vec4 forward;
    glm::vec4 extent;
    glm::vec4 params;
    glm::vec4 psf;
    glm::vec4 sun;
    glm::vec4 cloud_a;
    glm::vec4 cloud_b;
};
static_assert(sizeof(StarFrameGpu) == 224, "must match star_field.slang StarFrame");

// PSF width: a Gaussian of sigma 0.7 px is sampled above Nyquist (no
// aliasing as the star drifts across pixels) and its 5x5 footprint keeps
// 99.9 % of the energy before normalisation.
constexpr float kPsfSigmaPx = 0.7F;

// Below this cosine of the zenith angle the view is the planet surface
// (lib/atmosphere.slang sample_skyview's horizon at the 200 m view height).
[[nodiscard]] float horizon_cosine() {
    constexpr double ground = 6360.0;
    constexpr double view = 6360.2;
    return static_cast<float>(-std::sqrt(std::max(1.0 - (ground * ground) / (view * view), 0.0)));
}

// Hash in [0, 1) from a star index (decorrelates the twinkle of neighbours).
[[nodiscard]] float seed_of(std::uint32_t i) {
    std::uint32_t v = i * 747796405U + 2891336453U;
    v = ((v >> ((v >> 28U) + 4U)) ^ v) * 277803737U;
    v = (v >> 22U) ^ v;
    return static_cast<float>(v & 0xFFFFFFU) / 16777216.0F;
}

void buffer_barrier(VkCommandBuffer cmd, std::span<const VkBuffer> buffers,
                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    std::array<VkBufferMemoryBarrier2, 3> barriers{};
    const std::size_t n = std::min(buffers.size(), barriers.size());
    for (std::size_t i = 0; i < n; ++i) {
        VkBufferMemoryBarrier2& b = barriers[i];
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buffers[i];
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
    }
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = static_cast<std::uint32_t>(n);
    dep.pBufferMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

[[nodiscard]] std::expected<rhi::ManagedPipeline, core::Error>
make_pipeline(rhi::PipelineCache& cache, const rhi::LoadedShader* shader, const char* entry,
              const rhi::DescriptorSetLayout& layout) {
    const VkDescriptorSetLayout set_layout = layout.handle();
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = shader;
    info.entry = entry;
    info.set_layouts = {&set_layout, 1};
    return cache.create_compute_pipeline(info);
}

[[nodiscard]] float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

} // namespace

std::expected<std::shared_ptr<const StarCatalogueGpu>, core::Error>
StarCatalogueGpu::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         std::span<const astro::CatalogueStar> stars) {
    if (stars.empty()) return fail("empty catalogue");
    std::vector<StarGpu> gpu;
    gpu.reserve(stars.size());
    for (std::size_t i = 0; i < stars.size(); ++i) {
        const astro::CatalogueStar& s = stars[i];
        gpu.push_back({glm::vec4(s.direction, static_cast<float>(astro::star_illuminance_lux(s.v_mag))),
                       glm::vec4(astro::star_rgb(s.b_v), seed_of(static_cast<std::uint32_t>(i)))});
    }
    const VkDeviceSize bytes = gpu.size() * sizeof(StarGpu);
    auto buffer = allocator.create_buffer(bytes, kStorage, VMA_MEMORY_USAGE_AUTO, kHostWrite);
    if (!buffer) return std::unexpected(buffer.error());
    std::memcpy(buffer->mapped(), gpu.data(), static_cast<std::size_t>(bytes));
    if (auto f = buffer->flush(); !f) return std::unexpected(f.error());
    auto out = std::make_shared<StarCatalogueGpu>(Key{});
    out->buffer_ = std::move(*buffer);
    out->address_ = rhi::buffer_device_address(device.handle(), out->buffer_);
    out->size_ = bytes;
    out->count_ = static_cast<std::uint32_t>(gpu.size());
    return std::shared_ptr<const StarCatalogueGpu>(std::move(out));
}

std::expected<StarFieldPass, core::Error>
StarFieldPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                      rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                      std::shared_ptr<const StarCatalogueGpu> catalogue) {
    if (catalogue == nullptr) return fail("no catalogue");
    StarFieldPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 9> bindings{{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kComputeStage},
        {0, 7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kComputeStage},
        {0, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kComputeStage},
    }};
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/star_field.spv");
    if (!shader) return std::unexpected(shader.error());
    auto splat = make_pipeline(cache, *shader, "splat_main", out.layout_);
    if (!splat) return std::unexpected(splat.error());
    out.splat_pipeline_ = std::move(*splat);
    auto resolve = make_pipeline(cache, *shader, "resolve_main", out.layout_);
    if (!resolve) return std::unexpected(resolve.error());
    out.resolve_pipeline_ = std::move(*resolve);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    auto constants = allocator.create_buffer(sizeof(StarFrameGpu),
                                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             VMA_MEMORY_USAGE_AUTO, kHostWrite);
    if (!constants) return std::unexpected(constants.error());
    out.constants_ = std::move(*constants);
    auto visible = allocator.create_buffer(catalogue->count() * kVisibleBytes, kStorage,
                                           VMA_MEMORY_USAGE_AUTO);
    if (!visible) return std::unexpected(visible.error());
    out.visible_ = std::move(*visible);
    out.catalogue_ = std::move(catalogue);
    return out;
}

std::expected<void, core::Error> StarFieldPass::ensure_tiles(VkDeviceSize tiles) {
    // Sized for the most tiles seen: a smaller extent reuses the buffers.
    if (tiles <= tile_slots_) return {};
    auto counts = allocator_->create_buffer(tiles * sizeof(std::uint32_t),
                                            kStorage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            VMA_MEMORY_USAGE_AUTO);
    if (!counts) return std::unexpected(counts.error());
    auto lists = allocator_->create_buffer(tiles * k_tile_capacity * sizeof(std::uint32_t),
                                           kStorage, VMA_MEMORY_USAGE_AUTO);
    if (!lists) return std::unexpected(lists.error());
    tile_counts_ = std::move(*counts);
    tile_lists_ = std::move(*lists);
    tile_slots_ = tiles;
    return {};
}

std::expected<void, core::Error>
StarFieldPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr,
                            rhi::ResourceHandle depth, VkImageView transmittance,
                            const StarFieldFrame& frame) {
    if (!valid()) return fail("pass not created");
    if (transmittance == VK_NULL_HANDLE) return fail("no transmittance LUT");
    if (frame.extent.width == 0 || frame.extent.height == 0) return {};
    const std::uint32_t tx = (frame.extent.width + k_tile - 1) / k_tile;
    const std::uint32_t ty = (frame.extent.height + k_tile - 1) / k_tile;
    if (auto t = ensure_tiles(static_cast<VkDeviceSize>(tx) * ty); !t) {
        return std::unexpected(t.error());
    }

    StarFrameGpu c{};
    for (int i = 0; i < 4; ++i) c.view_proj[i] = frame.view_proj[i];
    for (int i = 0; i < 3; ++i) c.world_from_celestial[i] = glm::vec4(frame.world_from_celestial[i], 0.0F);
    // Solid angle of the on-axis pixel: (2 / (W P00)) (2 / (H P11)).
    const float w = static_cast<float>(frame.extent.width);
    const float h = static_cast<float>(frame.extent.height);
    const float p00 = std::abs(frame.projection[0][0]);
    const float p11 = std::abs(frame.projection[1][1]);
    const float omega = 4.0F / (std::max(w * p00, 1e-6F) * std::max(h * p11, 1e-6F));
    const glm::vec3 forward = glm::normalize(frame.camera_forward);
    c.forward = glm::vec4(forward, omega);
    c.extent = glm::vec4(w, h, static_cast<float>(tx), static_cast<float>(ty));
    const float pre = frame.pre_exposure > 0.0F ? finite_or(frame.pre_exposure, 1.0F) : 1.0F;
    c.params = glm::vec4(pre / photometry::k_nits_per_unit, finite_or(frame.time_s, 0.0F),
                         std::clamp(finite_or(frame.scintillation, 1.0F), 0.0F, 4.0F),
                         static_cast<float>(catalogue_->count()));
    c.psf = glm::vec4(kPsfSigmaPx, 1.0F / (2.0F * kPsfSigmaPx * kPsfSigmaPx),
                      static_cast<float>(k_tile_capacity), horizon_cosine());
    const bool clouds_on = frame.clouds.enabled && frame.clouds.coverage > 0.001F;
    c.sun = glm::vec4(glm::normalize(frame.sun_direction), clouds_on ? 1.0F : 0.0F);
    c.cloud_a = frame.clouds.pack_a();
    c.cloud_b = frame.clouds.pack_b();
    std::memcpy(constants_.mapped(), &c, sizeof(c));
    if (auto f = constants_.flush(0, sizeof(c)); !f) return std::unexpected(f.error());

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    descriptor_ = std::move(*db);

    const VkDeviceSize tiles = static_cast<VkDeviceSize>(tx) * ty;
    const VkBuffer counts = tile_counts_.handle();
    const VkBuffer lists = tile_lists_.handle();
    const VkBuffer visible = visible_.handle();
    const VkDevice dev = device_->handle();
    const VkDeviceAddress constants_addr = rhi::buffer_device_address(dev, constants_);
    const VkDeviceAddress visible_addr = rhi::buffer_device_address(dev, visible_);
    const VkDeviceAddress counts_addr = rhi::buffer_device_address(dev, tile_counts_);
    const VkDeviceAddress lists_addr = rhi::buffer_device_address(dev, tile_lists_);
    const VkDeviceSize visible_size = catalogue_->count() * kVisibleBytes;
    const std::uint32_t star_count = catalogue_->count();
    const VkExtent2D extent = frame.extent;

    graph.add_pass("star_field", rhi::PassType::compute)
        .reads(depth, rhi::ResourceUsage::sampled_compute)
        .writes(hdr, rhi::ResourceUsage::storage_read_write)
        .execute([this, hdr, depth, transmittance, counts, lists, visible, constants_addr,
                  visible_addr, counts_addr, lists_addr, visible_size, tiles, star_count,
                  extent](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rhi::DescriptorBuffer& d = descriptor_;
            d.write_uniform_buffer(0, constants_addr, sizeof(StarFrameGpu));
            d.write_storage_buffer(1, catalogue_->address(), catalogue_->size());
            d.write_storage_buffer(2, visible_addr, visible_size);
            d.write_storage_buffer(3, counts_addr, tiles * sizeof(std::uint32_t));
            d.write_storage_buffer(4, lists_addr, tiles * k_tile_capacity * sizeof(std::uint32_t));
            d.write_sampled_image(5, transmittance, ro);
            d.write_sampler(6, sampler_.handle());
            d.write_sampled_image(7, ctx.resolve(depth).view, ro);
            d.write_storage_image(8, ctx.resolve(hdr).view, VK_IMAGE_LAYOUT_GENERAL);

            // The previous use of these buffers (this slot's last frame) is
            // fenced; order this frame's clear and writes after nothing else.
            vkCmdFillBuffer(cmd, counts, 0, tiles * sizeof(std::uint32_t), 0);
            const std::array<VkBuffer, 1> clear{counts};
            buffer_barrier(cmd, clear, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, splat_pipeline_.handle());
            d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, splat_pipeline_.layout(), 0);
            vkCmdDispatch(cmd, (star_count + 63) / 64, 1, 1);

            const std::array<VkBuffer, 3> binned{counts, lists, visible};
            buffer_barrier(cmd, binned, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_.handle());
            d.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_pipeline_.layout(), 0);
            vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        });
    return {};
}

} // namespace engine::renderer
