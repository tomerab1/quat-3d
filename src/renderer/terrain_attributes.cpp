// Per-tile terrain attributes baked on the GPU (see
// engine/renderer/terrain_attributes.hpp and terrain_attributes.slang).

#include "engine/renderer/terrain_attributes.hpp"

#include <array>
#include <utility>

#include "compute_kernel.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"terrain attributes: " + std::move(what)});
}

// Mirrors Push in terrain_attributes.slang.
struct AttributesPush {
    std::uint32_t resolution = 0;
    float         metres = 0.0F;
    std::uint32_t cur = 0;
    std::uint32_t pad = 0;
};

constexpr std::array<const char*, 4> k_entries = {"attr_weights", "attr_flow_init",
                                                  "attr_flow_step", "attr_finalize"};
constexpr std::array<rhi::BindingRequest, 4> k_resources{{
    {"g_height", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_flow", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_wsum", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr VkFormat k_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkPipelineStageFlags2 k_cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 k_rw =
    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

} // namespace

std::expected<TerrainAttributeBaker, core::Error>
TerrainAttributeBaker::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                              rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    TerrainAttributeBaker out;
    out.fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.fns_.valid()) return fail("descriptor buffer functions unavailable");
    auto shader = cache.load(cooked_shader_dir + "/terrain_attributes.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings =
        rhi::reflected_bindings((*shader)->reflection, k_resources, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!bindings) return fail(bindings.error().message);
    for (const rhi::DescriptorBinding& b : *bindings) out.slots_.push_back(b.binding);
    auto layout = rhi::DescriptorSetLayout::create(device, out.fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AttributesPush)};
    for (std::size_t k = 0; k < k_entries.size(); ++k) {
        rhi::ComputePipeline::CreateInfo info{};
        info.shader = *shader;
        info.entry = k_entries[k];
        info.set_layouts = {&set_layout, 1};
        info.push_constants = {&push, 1};
        auto pipeline = cache.create_compute_pipeline(info);
        if (!pipeline) return std::unexpected(pipeline.error());
        out.pipelines_[k] = std::move(*pipeline);
    }
    out.device_ = &device;
    out.allocator_ = &allocator;
    return out;
}

std::expected<TerrainAttributeBaker::Target, core::Error>
TerrainAttributeBaker::schedule(VkImageView heights, std::uint32_t resolution, float metres,
                                std::uint32_t sweeps) {
    if (!valid()) return fail("baker not created");
    if (resolution < 2 || resolution > max_resolution) return fail("tile resolution out of range");
    const VkDevice vk = device_->handle();
    Target t;
    auto image = allocator_->create_image(k_format, {resolution, resolution},
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image) return std::unexpected(image.error());
    t.image = std::move(*image);
    auto view = rhi::create_image_view(vk, t.image.handle(), k_format);
    if (!view) return std::unexpected(view.error());
    t.view = std::move(*view);

    Bake b;
    b.image = t.image.handle();
    b.resolution = resolution;
    b.metres = metres;
    b.sweeps = sweeps;
    const VkDeviceSize n = static_cast<VkDeviceSize>(resolution) * resolution;
    const auto storage = [&](VkDeviceSize bytes, rhi::GpuBuffer& out) -> std::expected<void, core::Error> {
        auto buf = allocator_->create_buffer(
            bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO);
        if (!buf) return std::unexpected(buf.error());
        out = std::move(*buf);
        return {};
    };
    if (auto r = storage(2 * n * sizeof(float), b.flow); !r) return std::unexpected(r.error());
    if (auto r = storage(n * sizeof(float), b.wsum); !r) return std::unexpected(r.error());
    auto set = rhi::DescriptorBuffer::create(*device_, *allocator_, fns_, layout_);
    if (!set) return std::unexpected(set.error());
    b.set = std::move(*set);
    b.set.write_sampled_image(slots_[0], heights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    b.set.write_storage_buffer(slots_[1], rhi::buffer_device_address(vk, b.flow),
                               2 * n * sizeof(float));
    b.set.write_storage_buffer(slots_[2], rhi::buffer_device_address(vk, b.wsum),
                               n * sizeof(float));
    b.set.write_storage_image(slots_[3], t.view.handle(), VK_IMAGE_LAYOUT_GENERAL);
    queued_.push_back(std::move(b));
    return t;
}

void TerrainAttributeBaker::cancel(VkImage image) {
    std::erase_if(queued_, [image](const Bake& b) { return b.image == image; });
}

void TerrainAttributeBaker::record(VkCommandBuffer cmd) {
    for (Bake& b : queued_) {
        detail::image_barrier(cmd, b.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, k_cs,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const std::uint32_t groups = detail::groups_of(b.resolution);
        AttributesPush push{b.resolution, b.metres, 0, 0};
        const auto run = [&](Kernel k) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[k].handle());
            b.set.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[k].layout(), 0);
            vkCmdPushConstants(cmd, pipelines_[k].layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(cmd, groups, groups, 1);
            detail::memory_barrier(cmd, k_cs, k_rw, k_cs, k_rw);
        };
        run(weights);
        run(flow_init);
        for (std::uint32_t s = 0; s < b.sweeps; ++s) {
            run(flow_step);
            push.cur = 1U - push.cur;
        }
        run(finalize);
        detail::image_barrier(cmd, b.image, VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_cs,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | k_cs |
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        b.frames_left = 3;
        recorded_.push_back(std::move(b));
    }
    queued_.clear();
}

void TerrainAttributeBaker::tick() {
    std::erase_if(recorded_, [](Bake& b) { return --b.frames_left < 0; });
}

} // namespace engine::renderer
