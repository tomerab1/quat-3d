#include "app/particle_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

namespace {

namespace r = engine::renderer;

constexpr std::uintmax_t k_max_flipbook_bytes = 64ULL << 20;
constexpr std::uint32_t  k_max_flipbook_extent = 8192;

// Flipbooks are project files under assets/ (the codec checks the same;
// editor edits reach here unchecked).
[[nodiscard]] bool flipbook_path_ok(const std::string& path) {
    const std::filesystem::path p(path);
    if (path.rfind("assets/", 0) != 0 || p.is_absolute() || p.has_root_name() ||
        path.find('\0') != std::string::npos) {
        return false;
    }
    return std::none_of(p.begin(), p.end(), [](const auto& part) { return part == ".."; });
}

} // namespace

ParticleRuntime::ParticleRuntime(const engine::rhi::Device& device,
                                 engine::rhi::GpuAllocator& allocator,
                                 engine::rhi::PipelineCache& cache,
                                 const engine::rhi::TransferContext& transfer,
                                 std::string shader_dir, std::string project_root,
                                 std::uint32_t frames_in_flight)
    : device_(device), allocator_(allocator), cache_(cache), transfer_(transfer),
      shader_dir_(std::move(shader_dir)), project_root_(std::move(project_root)),
      frames_in_flight_(frames_in_flight) {}

void ParticleRuntime::warn_once(const std::string& message) {
    if (message == last_warning_) return;
    last_warning_ = message;
    std::fprintf(stderr, "[warn] particles: %s\n", message.c_str());
}

bool ParticleRuntime::ensure_pass() {
    if (pass_) return true;
    if (create_failed_) return false;
    auto created = r::ParticlePass::create(device_, allocator_, cache_, transfer_, shader_dir_,
                                           frames_in_flight_, r::particle_default_pool);
    if (!created) {
        create_failed_ = true;
        std::fprintf(stderr, "[warn] particles disabled: %s\n", created.error().message.c_str());
        return false;
    }
    pass_ = std::move(*created);
    return true;
}

void ParticleRuntime::release(Emitter& e) {
    if (e.capacity > 0) pool_.free(e.base, e.capacity);
    if (e.capacity > 0 && e.slot < slot_used_.size()) slot_used_[e.slot] = false;
    e.capacity = 0;
}

bool ParticleRuntime::place(entt::entity entity, Emitter& e) {
    release(e);
    const auto free_slot = std::find(slot_used_.begin(), slot_used_.end(), false);
    const std::uint32_t capacity = e.settings.max_particles;
    const std::uint32_t base = free_slot == slot_used_.end() ? UINT32_MAX : pool_.allocate(capacity);
    if (base == UINT32_MAX) {
        warn_once("emitter " + std::to_string(static_cast<std::uint32_t>(entity)) +
                  " skipped: the particle budget (" + std::to_string(r::particle_default_pool) +
                  " particles, " + std::to_string(r::particle_max_emitters) +
                  " emitters) is used up");
        return false;
    }
    *free_slot = true;
    e.slot = static_cast<std::uint32_t>(free_slot - slot_used_.begin());
    e.base = base;
    e.capacity = capacity;
    e.space = e.settings.space;
    e.spawn = {};
    e.fresh = true;
    return true;
}

VkImageView ParticleRuntime::flipbook(const std::string& path) {
    if (path.empty()) return VK_NULL_HANDLE;
    auto [it, inserted] = flipbooks_.try_emplace(path);
    Flipbook& book = it->second;
    if (!inserted) return book.failed ? VK_NULL_HANDLE : book.texture.view.handle();
    book.failed = true;
    if (!flipbook_path_ok(path)) {
        warn_once("flipbook '" + path + "' is not a path under assets/");
        return VK_NULL_HANDLE;
    }
    const std::filesystem::path file = std::filesystem::path(project_root_) / path;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size == 0 || size > k_max_flipbook_bytes) {
        warn_once("flipbook '" + path + "' is missing or larger than 64 MiB");
        return VK_NULL_HANDLE;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        warn_once("flipbook '" + path + "' could not be read");
        return VK_NULL_HANDLE;
    }
    auto pixels = engine::scene::decode_image(bytes, engine::asset::TextureColorSpace::srgb);
    if (!pixels || pixels->width > k_max_flipbook_extent || pixels->height > k_max_flipbook_extent) {
        warn_once("flipbook '" + path + "': " +
                  (pixels ? std::string("larger than 8192 x 8192") : pixels.error().message));
        return VK_NULL_HANDLE;
    }
    auto texture = engine::scene::upload_texture(*pixels, allocator_, transfer_);
    if (!texture) {
        warn_once("flipbook '" + path + "': " + texture.error().message);
        return VK_NULL_HANDLE;
    }
    book.texture = std::move(*texture);
    book.failed = false;
    return book.texture.view.handle();
}

void ParticleRuntime::add_to_graph(engine::rhi::RenderGraph& graph, const entt::registry& registry,
                                   const ParticleFrameInputs& in) {
    const auto view =
        registry.view<const engine::scene::Transform, const engine::scene::ParticleEmitter>();
    if (emitters_.empty() && view.begin() == view.end()) return;
    if (in.gbuffer == nullptr || in.camera == nullptr || !ensure_pass()) return;

    // The simulation clock's step (0 while paused), bounded after a hitch.
    const float dt = std::isfinite(in.sim_dt) ? std::clamp(in.sim_dt, 0.0F, r::particle_max_step)
                                              : 0.0F;
    for (auto& [entity, e] : emitters_) e.seen = false;
    std::vector<r::ParticleEmitterInput> inputs;
    for (const auto [entity, transform, component] : view.each()) {
        auto [it, inserted] = emitters_.try_emplace(entity);
        Emitter& e = it->second;
        e.seen = true;
        e.settings = r::sanitize(component.settings);
        if (inserted || e.capacity != e.settings.max_particles || e.space != e.settings.space) {
            if (!place(entity, e)) continue;
        }
        r::ParticleEmitterInput input;
        input.settings = &e.settings;
        input.info.world = transform.world;
        input.info.slot = e.slot;
        input.info.pool_base = e.base;
        input.info.spawn_index = e.spawn.spawned;
        input.info.spawn_count = r::advance_spawn(e.spawn, e.settings, dt);
        input.info.reset = e.fresh;
        input.texture = flipbook(e.settings.texture);
        inputs.push_back(input);
    }
    // Emitters gone from the scene free their slot and pool range.
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        release(it->second);
        it = emitters_.erase(it);
    }
    // A stable order (slot) whatever the registry's iteration order.
    std::sort(inputs.begin(), inputs.end(),
              [](const r::ParticleEmitterInput& a, const r::ParticleEmitterInput& b) {
                  return a.info.slot < b.info.slot;
              });

    r::ParticleFrame frame;
    frame.emitters = inputs;
    frame.dt = dt;
    frame.view = in.camera->view;
    // The frame rasterises with the TAA-jittered matrix: particles must too,
    // to depth-test and soft-fade against the same depth buffer.
    frame.proj = in.jittered_view_proj * glm::inverse(in.camera->view);
    frame.camera = in.camera->position;
    frame.light = in.sun;
    frame.extent = in.extent;
    r::ParticleTargets targets;
    targets.depth = in.gbuffer->depth;
    targets.normal = in.gbuffer->normal;
    targets.hdr = in.hdr;
    if (in.lights != nullptr) targets.lights = *in.lights;
    if (auto out = pass_->add_to_graph(graph, in.slot, frame, targets); !out) {
        warn_once(out.error().message);
        return;
    }
    // Every placed emitter was in this frame: its reset is recorded.
    for (auto& [entity, e] : emitters_) e.fresh = e.fresh && e.capacity == 0;
}

} // namespace quat::app
