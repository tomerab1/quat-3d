#include "engine/renderer/hdri_environment.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "compute_kernel.hpp"
#include "engine/asset/hdr_decode.hpp"
#include "engine/renderer/hdri_math.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

using detail::ComputeKernel;
using detail::groups_of;
using detail::image_barrier;
using detail::memory_barrier;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"hdri environment: " + std::move(what)});
}

constexpr VkFormat k_source_format = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat k_map_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr std::uint32_t k_sh_floats4 = 69; // 6 faces x 10 partials + 9 final coefficients
constexpr std::uint32_t k_sh_final = 60;

// Push blocks mirroring the shaders.
struct ToCubePush {
    glm::vec4     rotation{0.0F}; // cos, sin, intensity, face size
    glm::vec4     sun_cone{0.0F}; // direction (panorama), cos(cone)
    float         sun_clamp = 0.0F;
    std::uint32_t src_width = 0;
    std::uint32_t src_height = 0;
    std::uint32_t pad = 0;
};
struct SizePush {
    std::uint32_t size = 0;
    std::uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
};
struct PrefilterPush {
    float         roughness = 0.0F;
    std::uint32_t mip_size = 0;
    float         env_size = 0.0F;
    std::uint32_t samples = 0;
};

constexpr std::array<rhi::BindingRequest, 2> k_to_cube{{
    {"g_src", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr std::array<rhi::BindingRequest, 2> k_downsample{{
    {"g_src", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_dst", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr std::array<rhi::BindingRequest, 3> k_prefilter{{
    {"g_env", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_env_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr std::array<rhi::BindingRequest, 2> k_sh_project{{
    {"g_src", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_sh", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};
constexpr std::array<rhi::BindingRequest, 2> k_sh_irradiance{{
    {"g_sh", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};
constexpr std::array<rhi::BindingRequest, 1> k_lut{{
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
}};

constexpr VkPipelineStageFlags2 k_cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 k_readers =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkAccessFlags2 k_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkAccessFlags2 k_read =
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
constexpr VkImageLayout k_general = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkImageLayout k_ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// The maps of one image, with their per-dispatch descriptors.
struct Maps {
    std::uint32_t src_width = 0;
    std::uint32_t src_height = 0;
    std::uint32_t face = 0;
    std::uint32_t env_mips = 0;
    std::uint32_t pre_face = 0;
    std::uint32_t sh_mip = 0;

    rhi::GpuImage                source;
    rhi::ImageView               source_view;
    rhi::GpuImage                env;
    rhi::ImageView               env_cube;   // every mip
    std::vector<rhi::ImageView>  env_levels; // 2D array per mip
    rhi::GpuImage                prefiltered;
    rhi::ImageView               pre_cube;
    std::vector<rhi::ImageView>  pre_levels;
    rhi::GpuImage                irradiance;
    rhi::ImageView               irr_cube;
    rhi::ImageView               irr_store;

    rhi::DescriptorBuffer              to_cube;
    std::vector<rhi::DescriptorBuffer> downsample; // mip i <- i - 1, from i = 1
    std::vector<rhi::DescriptorBuffer> prefilter;
    rhi::DescriptorBuffer              sh_project;
    rhi::DescriptorBuffer              sh_irradiance;
};

[[nodiscard]] std::expected<rhi::ImageView, core::Error>
array_view(VkDevice dev, VkImage image, std::uint32_t mip) {
    return rhi::create_image_view(dev, image, k_map_format, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                  VK_IMAGE_VIEW_TYPE_2D_ARRAY, mip, 0, 6);
}

[[nodiscard]] std::expected<rhi::ImageView, core::Error>
cube_view(VkDevice dev, VkImage image, std::uint32_t mips) {
    return rhi::create_image_view(dev, image, k_map_format, VK_IMAGE_ASPECT_COLOR_BIT, mips,
                                  VK_IMAGE_VIEW_TYPE_CUBE, 0, 0, 6);
}

} // namespace

struct HdriEnvironment::Impl {
    const rhi::Device* device = nullptr;
    rhi::GpuAllocator* allocator = nullptr;
    rhi::DescriptorBufferFunctions fns{};
    ComputeKernel to_cube, downsample, prefilter, sh_project, sh_irradiance, lut;
    rhi::Sampler          sampler; // trilinear clamp over every mip
    rhi::GpuImage         brdf_lut;
    rhi::ImageView        brdf_view;
    rhi::DescriptorBuffer lut_set;
    bool                  lut_baked = false;
    rhi::GpuBuffer        sh;
    VkDeviceAddress       sh_address = 0;

    std::optional<Maps>             maps;
    HdriSun                         sun;
    std::optional<HdriBakeSettings> baked_settings;

    [[nodiscard]] std::expected<rhi::DescriptorBuffer, core::Error>
    descriptor(const ComputeKernel& k) const {
        return detail::make_descriptor(*device, *allocator, fns, k);
    }
    [[nodiscard]] std::expected<void, core::Error> build_maps(Maps& m);
    void record(VkCommandBuffer cmd, const HdriBakeSettings& s, bool bake_lut);
};

std::uint32_t HdriEnvironment::face_size_for(std::uint32_t width) {
    const std::uint32_t quarter = std::max(width / 4, 1U);
    return std::clamp(std::bit_floor(quarter), k_min_face, k_max_face);
}

HdriEnvironment::HdriEnvironment() = default;
HdriEnvironment::~HdriEnvironment() = default;
HdriEnvironment::HdriEnvironment(HdriEnvironment&&) noexcept = default;
HdriEnvironment& HdriEnvironment::operator=(HdriEnvironment&&) noexcept = default;

std::expected<HdriEnvironment, core::Error>
HdriEnvironment::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    HdriEnvironment out;
    out.impl_ = std::make_unique<Impl>();
    Impl& s = *out.impl_;
    s.device = &device;
    s.allocator = &allocator;
    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("descriptor buffer functions unavailable");

    const auto build = [&](ComputeKernel& k, const char* shader,
                           std::span<const rhi::BindingRequest> req,
                           std::uint32_t push) -> std::expected<void, core::Error> {
        auto built = detail::build_compute_kernel(device, s.fns, cache, cooked_shader_dir, shader,
                                                  req, push, "hdri environment");
        if (!built) return std::unexpected(built.error());
        k = std::move(*built);
        return {};
    };
    for (auto r : {build(s.to_cube, "hdri_to_cube", k_to_cube, sizeof(ToCubePush)),
                   build(s.downsample, "cube_downsample", k_downsample, sizeof(SizePush)),
                   build(s.prefilter, "env_prefilter", k_prefilter, sizeof(PrefilterPush)),
                   build(s.sh_project, "sh_project", k_sh_project, sizeof(SizePush)),
                   build(s.sh_irradiance, "sh_irradiance", k_sh_irradiance, sizeof(SizePush)),
                   build(s.lut, "ibl_brdf_lut", k_lut, sizeof(SizePush))}) {
        if (!r) return std::unexpected(r.error());
    }

    // Enough LODs for the largest environment chain.
    const std::uint32_t max_mips = rhi::full_mip_levels({k_max_face, k_max_face});
    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge,
                                       max_mips);
    if (!sampler) return std::unexpected(sampler.error());
    s.sampler = std::move(*sampler);

    auto lut = allocator.create_image(k_map_format, {ibl_brdf_lut_size, ibl_brdf_lut_size},
                                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!lut) return std::unexpected(lut.error());
    s.brdf_lut = std::move(*lut);
    auto lut_view = rhi::create_image_view(device.handle(), s.brdf_lut.handle(), k_map_format);
    if (!lut_view) return std::unexpected(lut_view.error());
    s.brdf_view = std::move(*lut_view);
    auto lut_set = s.descriptor(s.lut);
    if (!lut_set) return std::unexpected(lut_set.error());
    s.lut_set = std::move(*lut_set);
    s.lut_set.write_storage_image(s.lut.slots[0], s.brdf_view.handle(), k_general);

    // Host-readable: tests and tools read the coefficients back.
    auto sh = allocator.create_buffer(
        k_sh_floats4 * sizeof(glm::vec4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!sh) return std::unexpected(sh.error());
    s.sh = std::move(*sh);
    std::memset(s.sh.mapped(), 0, k_sh_floats4 * sizeof(glm::vec4));
    if (auto f = s.sh.flush(); !f) return std::unexpected(f.error());
    s.sh_address = rhi::buffer_device_address(device.handle(), s.sh);
    return out;
}

std::expected<void, core::Error> HdriEnvironment::Impl::build_maps(Maps& m) {
    const VkDevice dev = device->handle();
    constexpr VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    constexpr VkImageCreateFlags cube = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    m.env_mips = rhi::full_mip_levels({m.face, m.face});
    m.pre_face = std::min(m.face, k_max_prefilter_face);
    m.sh_mip = static_cast<std::uint32_t>(
        std::countr_zero(m.face) - std::countr_zero(std::min(m.face, k_sh_face)));

    auto env = allocator->create_image(k_map_format, {m.face, m.face}, usage, m.env_mips, 6, cube);
    auto pre = allocator->create_image(k_map_format, {m.pre_face, m.pre_face}, usage,
                                       prefilter_mip_count, 6, cube);
    auto irr = allocator->create_image(k_map_format, {ibl_irradiance_size, ibl_irradiance_size},
                                       usage, 1, 6, cube);
    if (!env || !pre || !irr) return fail("map image creation failed");
    m.env = std::move(*env);
    m.prefiltered = std::move(*pre);
    m.irradiance = std::move(*irr);

    auto source_view = rhi::create_image_view(dev, m.source.handle(), k_source_format);
    auto env_cube = cube_view(dev, m.env.handle(), m.env_mips);
    auto pre_cube = cube_view(dev, m.prefiltered.handle(), prefilter_mip_count);
    auto irr_cube = cube_view(dev, m.irradiance.handle(), 1);
    auto irr_store = array_view(dev, m.irradiance.handle(), 0);
    if (!source_view || !env_cube || !pre_cube || !irr_cube || !irr_store) {
        return fail("map view creation failed");
    }
    m.source_view = std::move(*source_view);
    m.env_cube = std::move(*env_cube);
    m.pre_cube = std::move(*pre_cube);
    m.irr_cube = std::move(*irr_cube);
    m.irr_store = std::move(*irr_store);
    for (std::uint32_t mip = 0; mip < m.env_mips; ++mip) {
        auto v = array_view(dev, m.env.handle(), mip);
        if (!v) return std::unexpected(v.error());
        m.env_levels.push_back(std::move(*v));
    }
    for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
        auto v = array_view(dev, m.prefiltered.handle(), mip);
        if (!v) return std::unexpected(v.error());
        m.pre_levels.push_back(std::move(*v));
    }

    // Descriptors: the views never change for these maps, so write them once.
    auto to_cube_set = descriptor(to_cube);
    if (!to_cube_set) return std::unexpected(to_cube_set.error());
    m.to_cube = std::move(*to_cube_set);
    m.to_cube.write_sampled_image(to_cube.slots[0], m.source_view.handle(), k_ro);
    m.to_cube.write_storage_image(to_cube.slots[1], m.env_levels[0].handle(), k_general);
    for (std::uint32_t mip = 1; mip < m.env_mips; ++mip) {
        auto d = descriptor(downsample);
        if (!d) return std::unexpected(d.error());
        d->write_sampled_image(downsample.slots[0], m.env_levels[mip - 1].handle(), k_general);
        d->write_storage_image(downsample.slots[1], m.env_levels[mip].handle(), k_general);
        m.downsample.push_back(std::move(*d));
    }
    for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
        auto d = descriptor(prefilter);
        if (!d) return std::unexpected(d.error());
        d->write_sampled_image(prefilter.slots[0], m.env_cube.handle(), k_ro);
        d->write_sampler(prefilter.slots[1], sampler.handle());
        d->write_storage_image(prefilter.slots[2], m.pre_levels[mip].handle(), k_general);
        m.prefilter.push_back(std::move(*d));
    }
    auto project = descriptor(sh_project);
    auto irradiance_set = descriptor(sh_irradiance);
    if (!project || !irradiance_set) return fail("descriptor creation failed");
    m.sh_project = std::move(*project);
    m.sh_project.write_sampled_image(sh_project.slots[0], m.env_levels[m.sh_mip].handle(), k_ro);
    m.sh_project.write_storage_buffer(sh_project.slots[1], sh_address,
                                      k_sh_floats4 * sizeof(glm::vec4));
    m.sh_irradiance = std::move(*irradiance_set);
    m.sh_irradiance.write_storage_buffer(sh_irradiance.slots[0], sh_address,
                                         k_sh_floats4 * sizeof(glm::vec4));
    m.sh_irradiance.write_storage_image(sh_irradiance.slots[1], m.irr_store.handle(), k_general);
    return {};
}

std::expected<void, core::Error> HdriEnvironment::set_image(const asset::HdrImage& image,
                                                            const rhi::TransferContext& transfer) {
    if (!impl_) return fail("not created");
    Impl& s = *impl_;
    const std::span<const float> rgb = image.rgb();
    if (image.width == 0 || image.height == 0 ||
        rgb.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
        return fail("empty or inconsistent image");
    }
    // RGB -> RGBA32F (RGB32F is not a required sampled format). Non-finite
    // or negative texels of a hostile file become 0.
    std::vector<float> rgba(static_cast<std::size_t>(image.width) * image.height * 4);
    for (std::size_t i = 0, n = rgba.size() / 4; i < n; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const float v = rgb[i * 3 + c];
            rgba[i * 4 + c] = std::isfinite(v) ? std::max(v, 0.0F) : 0.0F;
        }
        rgba[i * 4 + 3] = 1.0F;
    }
    // Frames in flight may still sample the previous maps.
    if (s.maps) vkDeviceWaitIdle(s.device->handle());
    s.maps.reset();
    s.baked_settings.reset();

    Maps m;
    m.src_width = image.width;
    m.src_height = image.height;
    m.face = face_size_for(image.width);
    auto source = rhi::upload_device_image(*s.allocator, transfer, rgba.data(),
                                           rgba.size() * sizeof(float),
                                           {image.width, image.height}, k_source_format);
    if (!source) return std::unexpected(source.error());
    m.source = std::move(*source);
    if (auto built = s.build_maps(m); !built) return std::unexpected(built.error());
    s.sun = analyze_hdri_sun(image);
    s.maps = std::move(m);
    return {};
}

void HdriEnvironment::clear() {
    if (!impl_) return;
    if (impl_->maps) vkDeviceWaitIdle(impl_->device->handle());
    impl_->maps.reset();
    impl_->baked_settings.reset();
    impl_->sun = {};
}

bool HdriEnvironment::has_image() const { return impl_ && impl_->maps.has_value(); }

bool HdriEnvironment::baked() const { return has_image() && impl_->baked_settings.has_value(); }

void HdriEnvironment::Impl::record(VkCommandBuffer cmd, const HdriBakeSettings& st,
                                   bool bake_lut) {
    Maps& m = *maps;
    constexpr VkPipelineStageFlags2 all = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // Earlier frames may still sample the previous bake: wait for them, then
    // discard the old contents.
    image_barrier(cmd, m.env.handle(), VK_IMAGE_LAYOUT_UNDEFINED, k_general, all, 0, k_cs,
                  k_write | k_read, m.env_mips, 6);
    image_barrier(cmd, m.prefiltered.handle(), VK_IMAGE_LAYOUT_UNDEFINED, k_general, all, 0, k_cs,
                  k_write, prefilter_mip_count, 6);
    image_barrier(cmd, m.irradiance.handle(), VK_IMAGE_LAYOUT_UNDEFINED, k_general, all, 0, k_cs,
                  k_write, 1, 6);
    memory_barrier(cmd, all, 0, k_cs, k_write | k_read);

    // 1) panorama -> cube mip 0.
    const float rot = glm::radians(st.rotation_deg);
    const bool remove = st.remove_sun && sun.found;
    ToCubePush tc;
    tc.rotation = {std::cos(rot), std::sin(rot), std::max(st.intensity, 0.0F),
                   static_cast<float>(m.face)};
    tc.sun_cone = {sun.direction, remove ? sun.cone_cos : 2.0F};
    tc.sun_clamp = sun.clamp_luminance;
    tc.src_width = m.src_width;
    tc.src_height = m.src_height;
    to_cube.dispatch(cmd, m.to_cube, &tc, groups_of(m.face), groups_of(m.face), 6);

    // 2) mip chain.
    for (std::uint32_t mip = 1; mip < m.env_mips; ++mip) {
        memory_barrier(cmd, k_cs, k_write, k_cs, k_read);
        const SizePush p{std::max(m.face >> mip, 1U)};
        downsample.dispatch(cmd, m.downsample[mip - 1], &p, groups_of(p.size), groups_of(p.size),
                            6);
    }
    image_barrier(cmd, m.env.handle(), k_general, k_ro, k_cs, k_write, k_readers, k_read,
                  m.env_mips, 6);

    // 3) prefiltered specular, roughness = mip / (count - 1).
    for (std::uint32_t mip = 0; mip < prefilter_mip_count; ++mip) {
        PrefilterPush p;
        p.roughness = static_cast<float>(mip) / static_cast<float>(prefilter_mip_count - 1);
        p.mip_size = std::max(m.pre_face >> mip, 1U);
        p.env_size = static_cast<float>(m.face);
        p.samples = k_prefilter_samples;
        prefilter.dispatch(cmd, m.prefilter[mip], &p, groups_of(p.mip_size),
                           groups_of(p.mip_size), 6);
    }
    image_barrier(cmd, m.prefiltered.handle(), k_general, k_ro, k_cs, k_write, k_readers, k_read,
                  prefilter_mip_count, 6);

    // 4) SH9 irradiance.
    const SizePush project{std::max(m.face >> m.sh_mip, 1U)};
    sh_project.dispatch(cmd, m.sh_project, &project, 6, 1, 1);
    memory_barrier(cmd, k_cs, k_write, k_cs, k_write | k_read);
    const SizePush irr{ibl_irradiance_size};
    sh_irradiance.dispatch(cmd, m.sh_irradiance, &irr, groups_of(irr.size), groups_of(irr.size),
                           6);
    image_barrier(cmd, m.irradiance.handle(), k_general, k_ro, k_cs, k_write, k_readers, k_read, 1,
                  6);
    memory_barrier(cmd, k_cs, k_write, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

    // 5) the DFG LUT, once.
    if (bake_lut) {
        image_barrier(cmd, brdf_lut.handle(), VK_IMAGE_LAYOUT_UNDEFINED, k_general,
                      VK_PIPELINE_STAGE_2_NONE, 0, k_cs, k_write);
        const SizePush p{ibl_brdf_lut_size};
        lut.dispatch(cmd, lut_set, &p, groups_of(p.size), groups_of(p.size), 1);
        image_barrier(cmd, brdf_lut.handle(), k_general, k_ro, k_cs, k_write, k_readers, k_read);
    }
}

std::expected<bool, core::Error> HdriEnvironment::add_bake_to_graph(rhi::RenderGraph& graph,
                                                                    const HdriBakeSettings& settings) {
    if (!has_image()) return false;
    Impl& s = *impl_;
    if (s.baked_settings && *s.baked_settings == settings) return false;
    s.baked_settings = settings;
    const bool bake_lut = !s.lut_baked;
    s.lut_baked = true;
    Impl* impl = impl_.get();
    graph.add_pass("hdri_bake", rhi::PassType::compute)
        .execute([impl, settings, bake_lut](rhi::PassContext& ctx) {
            impl->record(ctx.cmd(), settings, bake_lut);
        });
    return true;
}

IblViewSet HdriEnvironment::views() const {
    if (!baked()) return {};
    const Maps& m = *impl_->maps;
    return {m.irr_cube.handle(), m.pre_cube.handle(), impl_->brdf_view.handle(),
            impl_->sampler.handle()};
}

VkImageView HdriEnvironment::environment_view() const {
    return baked() ? impl_->maps->env_cube.handle() : VK_NULL_HANDLE;
}

const HdriSun& HdriEnvironment::sun() const {
    static const HdriSun none{};
    return impl_ ? impl_->sun : none;
}

HdriSunLight HdriEnvironment::sun_light(const HdriBakeSettings& settings) const {
    HdriSunLight out;
    const HdriSun& s = sun();
    if (!s.found) return out;
    out.found = true;
    out.direction_to_sun =
        glm::normalize(panorama_to_world(s.direction, glm::radians(settings.rotation_deg)));
    const glm::vec3 e = s.irradiance * std::max(settings.intensity, 0.0F);
    const float peak = std::max({e.r, e.g, e.b});
    if (peak > 0.0F) {
        out.color = e / peak;
        out.intensity = peak;
    }
    return out;
}

std::uint32_t HdriEnvironment::face_size() const { return has_image() ? impl_->maps->face : 0; }
std::uint32_t HdriEnvironment::prefilter_face_size() const {
    return has_image() ? impl_->maps->pre_face : 0;
}
VkImage HdriEnvironment::environment_image() const {
    return has_image() ? impl_->maps->env.handle() : VK_NULL_HANDLE;
}
VkImage HdriEnvironment::prefiltered_image() const {
    return has_image() ? impl_->maps->prefiltered.handle() : VK_NULL_HANDLE;
}
VkImage HdriEnvironment::irradiance_image() const {
    return has_image() ? impl_->maps->irradiance.handle() : VK_NULL_HANDLE;
}
VkImage HdriEnvironment::brdf_lut_image() const {
    return impl_ ? impl_->brdf_lut.handle() : VK_NULL_HANDLE;
}

std::array<glm::vec3, 9> HdriEnvironment::sh9() const {
    std::array<glm::vec3, 9> out{};
    if (!impl_) return out;
    if (auto r = impl_->sh.invalidate_mapped(); !r) return out;
    const auto* v = static_cast<const glm::vec4*>(impl_->sh.mapped());
    for (std::uint32_t k = 0; k < 9; ++k) out[k] = glm::vec3(v[k_sh_final + k]);
    return out;
}

} // namespace engine::renderer
