#include "engine/renderer/terrain_synth_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t kGroupSize = 8; // terrain_synth.slang numthreads(8, 8, 1)
constexpr int kRetireFrames = 3;        // outlive every frame in flight

// Mirrors SynthParams in terrain_synth.slang (std140, 128 bytes).
struct SynthUniforms {
    glm::ivec4 tile{0};  // world tile x, z; resolution; octaves
    glm::vec4  noise{0}; // noise texel scale, base frequency, lacunarity, gain
    glm::vec4  shape{0}; // warp strength, height (m)
    glm::vec4  step0{0}; // dt, rain * dt, pipe, cell area
    glm::vec4  step1{0}; // 1 / area, cell, 1 / cell, cell * sqrt(2)
    glm::vec4  step2{0}; // advect, Ks * dt, Kd * dt, 1 - Ke * dt
    glm::vec4  step3{0}; // thermal / 8, 1 / max depth, capacity, min tilt
    glm::vec4  step4{0}; // talus
};
static_assert(sizeof(SynthUniforms) == 128, "must match terrain_synth.slang SynthParams");

// Mirrors Push in terrain_synth.slang.
struct SynthPush {
    std::uint32_t cur = 0;
};

enum Kernel : std::uint8_t { k_init, k_flux, k_water, k_apply, k_transport, k_finalize, k_count };
constexpr std::array<const char*, k_count> kEntries{
    "synth_init", "erode_flux", "erode_water", "erode_apply", "erode_transport", "synth_finalize"};

// terrain_synth.slang's resources, by name; their binding slots come from the
// shader's reflection (State::slots, indexed by Resource).
enum Resource : std::uint8_t {
    r_params, r_perm, r_height, r_water, r_sediment, r_flux, r_flow, r_out, r_count
};
constexpr std::array<rhi::BindingRequest, r_count> kResources{{
    {"g_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_perm", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_height", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_water", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_sediment", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_flux", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_flow", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

[[nodiscard]] SynthUniforms make_uniforms(const terrain::TerrainParams& p, std::uint32_t res) {
    SynthUniforms u;
    u.tile = glm::ivec4(p.world_tile.x, p.world_tile.y, static_cast<std::int32_t>(res),
                        p.octaves);
    u.noise = glm::vec4(terrain::noise_texel_scale(p), p.base_frequency, p.lacunarity, p.gain);
    u.shape = glm::vec4(p.warp_strength, p.height_m, 0.0F, 0.0F);
    const float cell = p.tile_size_m / static_cast<float>(res - 1); // Heightmap::metres_per_texel
    const terrain::GridErosionStep k = terrain::grid_erosion_step(p.grid, cell);
    u.step0 = glm::vec4(k.dt, k.rain_dt, k.pipe, k.area);
    u.step1 = glm::vec4(k.inv_area, k.cell, k.inv_cell, k.diagonal);
    u.step2 = glm::vec4(k.advect, k.erode_dt, k.deposit_dt, k.keep_water);
    u.step3 = glm::vec4(k.thermal, k.inv_max_depth, k.capacity, k.min_tilt);
    u.step4 = glm::vec4(k.talus, 0.0F, 0.0F, 0.0F);
    return u;
}

void compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

// ---------------------------------------------------------------------------
// Job / State
// ---------------------------------------------------------------------------
struct TerrainSynthPass::Job {
    std::uint64_t          id = 0;
    terrain::TerrainParams params{};
    std::uint32_t          res = 0;
    std::uint32_t          steps = 0;     // erosion steps in total
    std::uint32_t          next_step = 0; // first step not yet recorded
    bool                   initialised = false;

    VkDevice device = VK_NULL_HANDLE;
    VkEvent  done = VK_NULL_HANDLE; // set by the GPU after the readback copy

    rhi::GpuBuffer        uniforms; // host-visible SynthUniforms
    rhi::GpuBuffer        perm;     // host-visible uint[512]
    rhi::GpuBuffer        height, water, sediment, flux, flow, out;
    rhi::GpuBuffer        readback; // host-visible float[n]
    rhi::GpuImage         image;
    rhi::DescriptorBuffer descriptor;

    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() {
        if (done != VK_NULL_HANDLE) vkDestroyEvent(device, done, nullptr);
    }

    [[nodiscard]] std::size_t texels() const { return static_cast<std::size_t>(res) * res; }
};

struct TerrainSynthPass::State {
    const rhi::Device*                    device = nullptr;
    rhi::GpuAllocator*                    allocator = nullptr;
    rhi::DescriptorBufferFunctions        fns{};
    rhi::DescriptorSetLayout              layout;
    std::array<std::uint32_t, r_count>    slots{}; // binding of each Resource (reflected)
    std::array<rhi::ManagedPipeline, k_count> pipelines; // hot-reloadable (R2)
    VkDeviceSize                          max_storage_range = 0;

    std::uint64_t cell_budget = default_cell_budget;
    std::uint64_t next_id = 1;

    std::deque<std::unique_ptr<Job>>  queued;
    std::unique_ptr<Job>              active;   // started; slices being recorded
    std::vector<std::unique_ptr<Job>> awaiting; // fully recorded; readback pending
    std::vector<std::unique_ptr<Job>> failed;   // could not start
    std::vector<std::pair<int, std::unique_ptr<Job>>> retired;

    [[nodiscard]] std::expected<void, core::Error> start(Job& job);
    void record_slice(VkCommandBuffer cmd, Job& job, bool init, std::uint32_t first_step,
                      std::uint32_t step_count, bool finalize) const;
};

std::expected<void, core::Error> TerrainSynthPass::State::start(Job& job) {
    const std::size_t n = job.texels();
    const VkDevice vk = device->handle();
    job.device = vk;

    const auto host = [&](VkDeviceSize size, VkBufferUsageFlags usage) {
        return allocator->create_buffer(size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        VMA_MEMORY_USAGE_AUTO,
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    };
    const auto local = [&](rhi::GpuBuffer& out, VkDeviceSize size,
                           VkBufferUsageFlags extra) -> std::expected<void, core::Error> {
        auto b = allocator->create_buffer(size,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra,
                                          VMA_MEMORY_USAGE_AUTO);
        if (!b) return std::unexpected(b.error());
        out = std::move(*b);
        return {};
    };

    auto uniforms = host(sizeof(SynthUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!uniforms) return std::unexpected(uniforms.error());
    job.uniforms = std::move(*uniforms);
    const SynthUniforms u = make_uniforms(job.params, job.res);
    std::memcpy(job.uniforms.mapped(), &u, sizeof(u));
    if (auto f = job.uniforms.flush(); !f) return std::unexpected(f.error());

    auto perm = host(512 * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!perm) return std::unexpected(perm.error());
    job.perm = std::move(*perm);
    const std::array<std::uint8_t, 256> table = terrain::noise_permutation(job.params.seed);
    auto* words = static_cast<std::uint32_t*>(job.perm.mapped());
    for (std::size_t i = 0; i < 512; ++i) words[i] = table[i & 255U];
    if (auto f = job.perm.flush(); !f) return std::unexpected(f.error());

    const VkDeviceSize scalar = n * sizeof(float);
    const VkDeviceSize vector = n * 4 * sizeof(float);
    if (auto r = local(job.height, scalar, 0); !r) return r;
    if (auto r = local(job.water, scalar, 0); !r) return r;
    if (auto r = local(job.sediment, 2 * scalar, 0); !r) return r;
    if (auto r = local(job.flux, vector, 0); !r) return r;
    if (auto r = local(job.flow, vector, 0); !r) return r;
    if (auto r = local(job.out, scalar, VK_BUFFER_USAGE_TRANSFER_SRC_BIT); !r) return r;

    auto readback = allocator->create_buffer(
        scalar, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    job.readback = std::move(*readback);

    // TRANSFER_SRC lets tests and tools read the tile back.
    auto image = allocator->create_image(VK_FORMAT_R32_SFLOAT, {job.res, job.res},
                                         VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image) return std::unexpected(image.error());
    job.image = std::move(*image);

    auto descriptor = rhi::DescriptorBuffer::create(*device, *allocator, fns, layout);
    if (!descriptor) return std::unexpected(descriptor.error());
    job.descriptor = std::move(*descriptor);
    const auto storage = [&](Resource r, const rhi::GpuBuffer& buffer, VkDeviceSize range) {
        job.descriptor.write_storage_buffer(slots[r], rhi::buffer_device_address(vk, buffer),
                                            range);
    };
    job.descriptor.write_uniform_buffer(slots[r_params],
                                        rhi::buffer_device_address(vk, job.uniforms),
                                        sizeof(SynthUniforms));
    storage(r_perm, job.perm, 512 * sizeof(std::uint32_t));
    storage(r_height, job.height, scalar);
    storage(r_water, job.water, scalar);
    storage(r_sediment, job.sediment, 2 * scalar);
    storage(r_flux, job.flux, vector);
    storage(r_flow, job.flow, vector);
    storage(r_out, job.out, scalar);

    VkEventCreateInfo ei{};
    ei.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    if (vkCreateEvent(vk, &ei, nullptr, &job.done) != VK_SUCCESS) {
        return fail("terrain synth: vkCreateEvent failed");
    }
    return {};
}

void TerrainSynthPass::State::record_slice(VkCommandBuffer cmd, Job& job, bool init,
                                           std::uint32_t first_step, std::uint32_t step_count,
                                           bool finalize) const {
    const std::uint32_t groups = (job.res + kGroupSize - 1) / kGroupSize;
    bool bound = false;
    bool pending_barrier = false;
    const auto dispatch = [&](Kernel kernel, std::uint32_t cur) {
        if (pending_barrier) compute_barrier(cmd);
        const rhi::ManagedPipeline& pipeline = pipelines[kernel];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
        if (!bound) {
            // Every kernel shares the set layout and push range, so the
            // binding survives pipeline switches (compatible layouts).
            job.descriptor.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0);
            bound = true;
        }
        const SynthPush push{cur};
        vkCmdPushConstants(cmd, pipeline.layout(), kComputeStage, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, groups, 1);
        pending_barrier = true;
    };

    if (init) dispatch(k_init, 0);
    for (std::uint32_t s = first_step; s < first_step + step_count; ++s) {
        const std::uint32_t cur = s & 1U; // sediment ping-pong half
        dispatch(k_flux, cur);
        dispatch(k_water, cur);
        dispatch(k_apply, cur);
        dispatch(k_transport, cur);
    }
    if (finalize && job.steps > 0) dispatch(k_finalize, job.steps & 1U);
}

// ---------------------------------------------------------------------------
// TerrainSynthPass
// ---------------------------------------------------------------------------
TerrainSynthPass::TerrainSynthPass() = default;
TerrainSynthPass::~TerrainSynthPass() = default;
TerrainSynthPass::TerrainSynthPass(TerrainSynthPass&&) noexcept = default;
TerrainSynthPass& TerrainSynthPass::operator=(TerrainSynthPass&&) noexcept = default;

std::expected<TerrainSynthPass, core::Error>
TerrainSynthPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                         rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    TerrainSynthPass out;
    out.state_ = std::make_unique<State>();
    State& s = *out.state_;
    s.device = &device;
    s.allocator = &allocator;
    s.max_storage_range = device.properties().limits.maxStorageBufferRange;

    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("terrain synth: descriptor buffer functions unavailable");

    auto shader = cache.load(cooked_shader_dir + "/terrain_synth.spv");
    if (!shader) return std::unexpected(shader.error());
    // The set layout and every descriptor slot come from the shader's reflection.
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kComputeStage);
    if (!bindings) return fail("terrain synth: " + bindings.error().message);
    for (std::size_t r = 0; r < r_count; ++r) s.slots[r] = (*bindings)[r].binding;
    auto layout = rhi::DescriptorSetLayout::create(device, s.fns, *bindings);
    if (!layout) return std::unexpected(layout.error());
    s.layout = std::move(*layout);

    const VkDescriptorSetLayout set_layout = s.layout.handle();
    const VkPushConstantRange push{kComputeStage, 0, sizeof(SynthPush)};
    for (std::size_t k = 0; k < k_count; ++k) {
        rhi::ComputePipeline::CreateInfo info{};
        info.shader = *shader;
        info.entry = kEntries[k];
        info.set_layouts = {&set_layout, 1};
        info.push_constants = {&push, 1};
        auto pipeline = cache.create_compute_pipeline(info);
        if (!pipeline) return std::unexpected(pipeline.error());
        s.pipelines[k] = std::move(*pipeline);
    }
    return out;
}

bool TerrainSynthPass::supports(const terrain::TerrainParams& params) const {
    if (!state_) return false;
    if (params.erosion_model == terrain::ErosionModel::droplets && params.erosion_droplets > 0) {
        return false; // serial droplets: CPU only
    }
    const std::uint32_t res = params.resolution;
    if (res < 2 || res > max_resolution || !(params.tile_size_m > 0.0F)) return false;
    const VkDeviceSize widest = static_cast<VkDeviceSize>(res) * res * 4 * sizeof(float);
    return widest <= state_->max_storage_range;
}

std::uint64_t TerrainSynthPass::enqueue(const terrain::TerrainParams& params) {
    if (!supports(params)) return 0;
    auto job = std::make_unique<Job>();
    job->id = state_->next_id++;
    job->params = params;
    job->res = params.resolution;
    // terrain::erode_grid skips grids smaller than 3x3; so do we.
    const bool erodes = params.erosion_model == terrain::ErosionModel::grid && job->res >= 3;
    job->steps = erodes ? params.grid.iterations : 0;
    const std::uint64_t id = job->id;
    state_->queued.push_back(std::move(job));
    return id;
}

void TerrainSynthPass::set_cell_budget(std::uint64_t cells_per_frame) {
    if (state_) state_->cell_budget = cells_per_frame;
}

bool TerrainSynthPass::idle() const {
    return !state_ || (state_->queued.empty() && !state_->active && state_->awaiting.empty() &&
                       state_->failed.empty());
}

std::expected<void, core::Error> TerrainSynthPass::add_to_graph(rhi::RenderGraph& graph) {
    if (!state_) return fail("terrain synth: not created");
    State& s = *state_;

    for (auto it = s.retired.begin(); it != s.retired.end();) {
        it = --it->first <= 0 ? s.retired.erase(it) : it + 1;
    }

    // Start the next job; one that cannot get its resources fails alone.
    while (!s.active && !s.queued.empty()) {
        std::unique_ptr<Job> job = std::move(s.queued.front());
        s.queued.pop_front();
        if (auto r = s.start(*job); !r) {
            s.failed.push_back(std::move(job));
            continue;
        }
        s.active = std::move(job);
    }
    if (!s.active) return {};

    Job* job = s.active.get();
    const std::uint64_t texels = job->texels();
    const std::uint32_t remaining = job->steps - job->next_step;
    std::uint32_t slice = remaining;
    if (s.cell_budget > 0) {
        const std::uint64_t affordable = std::max<std::uint64_t>(s.cell_budget / texels, 1);
        slice = static_cast<std::uint32_t>(std::min<std::uint64_t>(remaining, affordable));
    }
    const std::uint32_t first_step = job->next_step;
    const bool last = first_step + slice == job->steps;

    // Previous slices (earlier frames) wrote the scratch; the first access
    // waits for them. Host-written inputs need nothing (vkQueueSubmit).
    constexpr VkPipelineStageFlags2 prior_stage =
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    constexpr VkAccessFlags2 prior_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    const std::size_t n = job->texels();
    const VkDeviceSize scalar = n * sizeof(float);
    const VkDeviceSize vector = n * 4 * sizeof(float);
    const rhi::ResourceHandle uniforms =
        graph.import_buffer("terrain_synth_params", job->uniforms.handle(), sizeof(SynthUniforms));
    const rhi::ResourceHandle perm =
        graph.import_buffer("terrain_synth_perm", job->perm.handle(), 512 * sizeof(std::uint32_t));
    const std::array<rhi::ResourceHandle, 6> scratch{
        graph.import_buffer("terrain_synth_height", job->height.handle(), scalar, prior_stage,
                            prior_access),
        graph.import_buffer("terrain_synth_water", job->water.handle(), scalar, prior_stage,
                            prior_access),
        graph.import_buffer("terrain_synth_sediment", job->sediment.handle(), 2 * scalar,
                            prior_stage, prior_access),
        graph.import_buffer("terrain_synth_flux", job->flux.handle(), vector, prior_stage,
                            prior_access),
        graph.import_buffer("terrain_synth_flow", job->flow.handle(), vector, prior_stage,
                            prior_access),
        graph.import_buffer("terrain_synth_out", job->out.handle(), scalar, prior_stage,
                            prior_access),
    };
    const rhi::ResourceHandle out = scratch[5];

    auto synth = graph.add_pass("terrain_synth", rhi::PassType::compute);
    synth.reads(uniforms, rhi::ResourceUsage::buffer_read_compute)
        .reads(perm, rhi::ResourceUsage::buffer_read_compute);
    for (const rhi::ResourceHandle h : scratch) {
        synth.writes(h, rhi::ResourceUsage::buffer_write_compute);
    }
    const bool init = !job->initialised;
    synth.execute([state = &s, job, init, first_step, slice, last](rhi::PassContext& ctx) {
        state->record_slice(ctx.cmd(), *job, init, first_step, slice, last);
    });
    job->initialised = true;
    job->next_step = first_step + slice;
    if (!last) return {};

    // Last slice: heights -> the tile image (shader-read for the terrain
    // pass's vertex + fragment stages) and -> the host, then signal.
    const VkExtent2D extent{job->res, job->res};
    const rhi::ResourceHandle image =
        graph.import_image("terrain_synth_image", job->image.handle(), VK_NULL_HANDLE,
                           VK_FORMAT_R32_SFLOAT, extent, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_UNDEFINED);
    const rhi::ResourceHandle readback =
        graph.import_buffer("terrain_synth_readback", job->readback.handle(), scalar);

    (void)graph.add_pass("terrain_synth_upload", rhi::PassType::transfer)
        .reads(out, rhi::ResourceUsage::transfer_src)
        .writes(image, rhi::ResourceUsage::transfer_dst)
        .execute([out, image, extent](rhi::PassContext& ctx) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(ctx.cmd(), ctx.resolve(out).buffer, ctx.resolve(image).image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });

    (void)graph.add_pass("terrain_synth_publish", rhi::PassType::transfer)
        .reads(image, rhi::ResourceUsage::sampled_graphics)
        .reads(out, rhi::ResourceUsage::transfer_src)
        .writes(readback, rhi::ResourceUsage::transfer_dst)
        .execute([job, out, readback, scalar](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const VkBufferCopy copy{0, 0, scalar};
            vkCmdCopyBuffer(cmd, ctx.resolve(out).buffer, ctx.resolve(readback).buffer, 1, &copy);

            VkMemoryBarrier2 to_host{};
            to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            to_host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &to_host;
            vkCmdPipelineBarrier2(cmd, &dep);

            // Signalled once everything before it (the whole job) is done.
            VkMemoryBarrier2 all{};
            all.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            all.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkDependencyInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            signal.memoryBarrierCount = 1;
            signal.pMemoryBarriers = &all;
            vkCmdSetEvent2(cmd, job->done, &signal);
        });

    s.awaiting.push_back(std::move(s.active));
    return {};
}

std::vector<TerrainSynthPass::Result> TerrainSynthPass::collect() {
    std::vector<Result> results;
    if (!state_) return results;
    State& s = *state_;

    for (std::unique_ptr<Job>& job : s.failed) {
        Result r;
        r.id = job->id;
        r.tile = job->params.world_tile;
        results.push_back(std::move(r));
        s.retired.emplace_back(kRetireFrames, std::move(job));
    }
    s.failed.clear();

    for (auto it = s.awaiting.begin(); it != s.awaiting.end();) {
        Job& job = **it;
        if (vkGetEventStatus(job.device, job.done) != VK_EVENT_SET) {
            ++it;
            continue;
        }
        Result r;
        r.id = job.id;
        r.tile = job.params.world_tile;
        if (auto inv = job.readback.invalidate_mapped(); inv) {
            terrain::Heightmap& map = r.heights;
            map.resolution = job.res;
            map.tile_size_m = job.params.tile_size_m;
            map.heights.resize(job.texels());
            std::memcpy(map.heights.data(), job.readback.mapped(), job.texels() * sizeof(float));
            map.min_height = std::numeric_limits<float>::max();
            map.max_height = std::numeric_limits<float>::lowest();
            for (const float v : map.heights) {
                map.min_height = std::min(map.min_height, v);
                map.max_height = std::max(map.max_height, v);
            }
            r.image = std::move(job.image);
        }
        results.push_back(std::move(r));
        s.retired.emplace_back(kRetireFrames, std::move(*it));
        it = s.awaiting.erase(it);
    }
    return results;
}

} // namespace engine::renderer
