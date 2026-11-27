#include "app/particle_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/core/profile.hpp"

namespace quat::app {

namespace {

namespace r = engine::renderer;
using engine::core::Error;

constexpr std::uintmax_t        k_max_flipbook_bytes = 64ULL << 20;
constexpr std::uint32_t         k_max_flipbook_extent = 8192;
constexpr std::chrono::seconds  k_max_backoff{30};
constexpr std::chrono::seconds  k_poll_interval{1}; // failed files: change check

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

// Worker thread: read + decode (no GPU access).
[[nodiscard]] std::expected<engine::scene::TextureData, Error>
decode_flipbook(const std::filesystem::path& file, const std::string& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size == 0 || size > k_max_flipbook_bytes) {
        return std::unexpected(Error{"flipbook '" + path + "' is missing or larger than 64 MiB"});
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::unexpected(Error{"flipbook '" + path + "' could not be read"});
    }
    auto pixels = engine::scene::decode_image(bytes, engine::asset::TextureColorSpace::srgb);
    if (!pixels) return std::unexpected(Error{"flipbook '" + path + "': " + pixels.error().message});
    if (pixels->width > k_max_flipbook_extent || pixels->height > k_max_flipbook_extent) {
        return std::unexpected(Error{"flipbook '" + path + "' is larger than 8192 x 8192"});
    }
    return pixels;
}

[[nodiscard]] std::filesystem::file_time_type stamp_of(const std::filesystem::path& file) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

} // namespace

ParticleRuntime::ParticleRuntime(const engine::rhi::Device& device,
                                 engine::rhi::GpuAllocator& allocator,
                                 engine::rhi::PipelineCache& cache,
                                 const engine::rhi::TransferContext& transfer,
                                 std::string shader_dir, std::filesystem::path project_root,
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
                                           frames_in_flight_, table_.pool_capacity());
    if (!created) {
        create_failed_ = true;
        std::fprintf(stderr, "[warn] particles disabled: %s\n", created.error().message.c_str());
        return false;
    }
    pass_ = std::move(*created);
    return true;
}

void ParticleRuntime::start_load(const std::string& path, Flipbook& book) {
    const std::filesystem::path file = project_root_ / path;
    book.failed = false;
    book.stamp = stamp_of(file);
    book.pending = std::async(std::launch::async, [file, path] {
        QUAT_THREAD_NAME("asset load");
        QUAT_ZONE_N("decode_flipbook");
        return decode_flipbook(file, path);
    });
}

VkImageView ParticleRuntime::flipbook(const std::string& path, Clock::time_point now) {
    if (path.empty()) return VK_NULL_HANDLE;
    auto [it, inserted] = flipbooks_.try_emplace(path);
    Flipbook& book = it->second;
    if (inserted) {
        if (!flipbook_path_ok(path)) {
            warn_once("flipbook '" + path + "' is not a path under assets/");
            book.failed = true;
            book.retry_at = Clock::time_point::max(); // a bad path never becomes good
            book.next_poll = Clock::time_point::max();
            return VK_NULL_HANDLE;
        }
        start_load(path, book);
    }
    if (book.ready) return book.texture.view.handle();
    if (book.pending.valid()) {
        if (book.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return VK_NULL_HANDLE; // the disc until it is decoded
        }
        Decoded decoded = book.pending.get();
        std::expected<engine::asset::TextureAsset, Error> texture =
            decoded ? engine::scene::upload_texture(*decoded, allocator_, transfer_)
                    : std::expected<engine::asset::TextureAsset, Error>(
                          std::unexpected(decoded.error()));
        if (texture) {
            book.texture = std::move(*texture);
            book.ready = true;
            return book.texture.view.handle();
        }
        warn_once(texture.error().message + " (retrying in " +
                  std::to_string(book.backoff.count()) + " s or when the file changes)");
        book.failed = true;
        book.retry_at = now + book.backoff;
        book.next_poll = now + k_poll_interval;
        book.backoff = std::min(book.backoff * 2, k_max_backoff);
        return VK_NULL_HANDLE;
    }
    // Failed: retry after the back-off, or early when the file changed.
    if (book.failed && now >= book.next_poll) {
        book.next_poll = now + k_poll_interval;
        const bool changed =
            stamp_of(project_root_ / path) != book.stamp;
        if (changed) book.backoff = std::chrono::seconds(1);
        if (changed || now >= book.retry_at) start_load(path, book);
    }
    return VK_NULL_HANDLE;
}

void ParticleRuntime::add_to_graph(engine::rhi::RenderGraph& graph, const entt::registry& registry,
                                   const ParticleFrameInputs& in) {
    const auto view =
        registry.view<const engine::scene::Transform, const engine::scene::ParticleEmitter>();
    if (table_.size() == 0 && view.begin() == view.end()) return;
    if (in.gbuffer == nullptr || in.camera == nullptr || !ensure_pass()) return;

    // The simulation clock's step (0 while paused), bounded after a hitch.
    const float dt = std::isfinite(in.sim_dt) ? std::clamp(in.sim_dt, 0.0F, r::particle_max_step)
                                              : 0.0F;
    const Clock::time_point now = Clock::now();
    table_.begin_frame();
    frame_settings_.clear();
    std::vector<r::ParticleEmitterInput> inputs;
    for (const auto [entity, transform, component] : view.each()) {
        const r::ParticleEmitterSettings& settings =
            frame_settings_.emplace_back(r::sanitize(component.settings));
        const auto key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(entity));
        const std::optional<r::EmitterFrameInfo> info =
            table_.visit(key, settings, transform.world, dt);
        if (!info) {
            warn_once("emitter " + std::to_string(key) + " waits: the particle budget (" +
                      std::to_string(table_.pool_capacity()) + " particles, " +
                      std::to_string(r::particle_max_emitters) + " emitters) is used up");
            continue;
        }
        r::ParticleEmitterInput input;
        input.settings = &settings;
        input.info = *info;
        input.texture = flipbook(settings.texture, now);
        inputs.push_back(input);
    }
    table_.end_frame(); // emitters gone from the scene free their slot and range
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
        warn_once(out.error().message); // resets stay pending: retried next frame
        return;
    }
    table_.commit();
}

} // namespace quat::app
