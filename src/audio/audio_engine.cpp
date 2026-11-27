// AudioEngine — see include/engine/audio/audio_engine.hpp. The only engine TU
// (besides miniaudio_impl.cpp) that sees miniaudio.

#include "engine/audio/audio_engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <miniaudio.h>

#include "engine/core/path_confine.hpp"

namespace engine::audio {

namespace {

// Slot indices live in the low 16 bits of a handle; index 0xFFFF is never
// used, so no live handle can equal invalid_sound.
constexpr std::size_t max_sounds = 0xFFFFU;

[[nodiscard]] SoundHandle make_handle(std::size_t index, std::uint16_t generation) {
    return (static_cast<SoundHandle>(generation) << 16U) | static_cast<SoundHandle>(index);
}

[[nodiscard]] float sanitize_gain(float v) { return std::isfinite(v) ? std::max(v, 0.0F) : 0.0F; }

[[nodiscard]] core::Error ma_error(std::string what, ma_result result) {
    return core::Error{"audio: " + std::move(what) + ": " + ma_result_description(result)};
}

[[nodiscard]] std::unexpected<core::Error> clip_error(const std::string& clip, std::string why) {
    return std::unexpected(core::Error{"audio: clip '" + clip + "': " + std::move(why)});
}

// The file a clip names (issue #66): inside `root`, an existing regular file,
// and small enough to decode into memory unless streamed. Checked before
// miniaudio opens anything, so a FIFO, device or /proc path can't block the
// main thread and a huge file is never decoded. Messages carry the clip as
// given, not the resolved path.
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
resolve_clip(const std::filesystem::path& asset_root, const std::string& clip, bool stream,
             std::uint64_t max_decoded_bytes) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = asset_root.empty() ? fs::current_path(ec) : asset_root;
    if (ec) return clip_error(clip, "cannot resolve the asset root: " + ec.message());
    auto path = core::confine_to_root(root, clip);
    if (!path) return clip_error(clip, path.error().message);
    const fs::file_status st = fs::status(*path, ec); // follows symlinks (confined above)
    if (!fs::exists(st)) return clip_error(clip, "no such file");
    if (!fs::is_regular_file(st)) return clip_error(clip, "not a regular file");
    const std::uintmax_t size = fs::file_size(*path, ec);
    if (ec) return clip_error(clip, "cannot read its size: " + ec.message());
    if (!stream && size > max_decoded_bytes) {
        return clip_error(clip, std::to_string(size >> 20U) + " MiB is over the " +
                                    std::to_string(max_decoded_bytes >> 20U) +
                                    " MiB limit for clips decoded into memory; use a streaming "
                                    "bus (music, ambience)");
    }
    return std::move(*path);
}

} // namespace

struct AudioEngine::Impl {
    struct Slot {
        std::unique_ptr<ma_sound> sound; // null while the slot is free
        std::uint16_t             generation = 0;
        std::uint32_t             owner = no_owner;
        bool                      paused = false;
    };

    // ma_engine / ma_sound_group hold self-referencing pointers: Impl lives on
    // the heap and never moves.
    ma_engine engine{};
    bool      engine_ready = false;
    std::array<ma_sound_group, bus_count> groups{}; // [master] unused: master is the engine
    std::size_t                           groups_ready = 0; // groups[1..groups_ready] are live
    bool                                  has_device = false;
    std::filesystem::path                 asset_root;
    std::uint64_t                         max_decoded_clip_bytes = 0;
    std::vector<Slot>                     slots;
    std::vector<std::uint16_t>            free_slots;
    std::size_t                           live = 0;
    ListenerPose                          listener;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        for (Slot& s : slots) release(s);
        for (std::size_t i = groups_ready; i > 0; --i) ma_sound_group_uninit(&groups[i]);
        if (engine_ready) ma_engine_uninit(&engine);
    }

    [[nodiscard]] Slot* find(SoundHandle h) {
        const std::size_t index = h & 0xFFFFU;
        if (h == invalid_sound || index >= slots.size()) return nullptr;
        Slot& s = slots[index];
        if (!s.sound || s.generation != static_cast<std::uint16_t>(h >> 16U)) return nullptr;
        return &s;
    }
    [[nodiscard]] const Slot* find(SoundHandle h) const {
        return const_cast<Impl*>(this)->find(h); // NOLINT: same lookup, read-only use
    }
    [[nodiscard]] ma_sound* sound(SoundHandle h) {
        Slot* s = find(h);
        return s != nullptr ? s->sound.get() : nullptr;
    }

    void release(Slot& s) {
        if (!s.sound) return;
        ma_sound_uninit(s.sound.get());
        s.sound.reset();
        s.owner = no_owner;
        s.paused = false;
        ++s.generation; // invalidates every outstanding handle to this slot
        --live;
    }

    [[nodiscard]] ma_sound_group* group(AudioBus bus) {
        const auto i = static_cast<std::size_t>(bus);
        return i == 0 || i > groups_ready ? nullptr : &groups[i];
    }
};

AudioEngine::AudioEngine(AudioEngine&&) noexcept = default;
AudioEngine& AudioEngine::operator=(AudioEngine&&) noexcept = default;
AudioEngine::~AudioEngine() = default;

std::expected<AudioEngine, core::Error> AudioEngine::create(const AudioEngineConfig& config) {
    AudioEngine out;
    out.impl_ = std::make_unique<Impl>();
    Impl& impl = *out.impl_;

    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;
    cfg.noDevice = config.no_device ? MA_TRUE : MA_FALSE;
    // Without a device miniaudio cannot infer the format; it must be explicit.
    cfg.sampleRate = config.sample_rate != 0 ? config.sample_rate : (config.no_device ? 48000U : 0U);
    cfg.channels = config.channels != 0 ? config.channels : (config.no_device ? 2U : 0U);
    if (const ma_result r = ma_engine_init(&cfg, &impl.engine); r != MA_SUCCESS) {
        return std::unexpected(ma_error(config.no_device ? "engine init failed"
                                                         : "no playback device",
                                        r));
    }
    impl.engine_ready = true;
    impl.has_device = !config.no_device;
    impl.asset_root = std::filesystem::path(config.asset_root);
    impl.max_decoded_clip_bytes = config.max_decoded_clip_bytes;

    // Sub-mix buses, each feeding the engine endpoint (= the master bus).
    for (std::size_t i = 1; i < bus_count; ++i) {
        if (const ma_result r = ma_sound_group_init(&impl.engine, 0, nullptr, &impl.groups[i]);
            r != MA_SUCCESS) {
            return std::unexpected(
                ma_error("bus '" + std::string(bus_names[i]) + "' init failed", r));
        }
        impl.groups_ready = i;
    }
    out.set_listener(ListenerPose{});
    return out;
}

bool AudioEngine::has_device() const { return impl_->has_device; }
std::uint32_t AudioEngine::sample_rate() const { return ma_engine_get_sample_rate(&impl_->engine); }
std::uint32_t AudioEngine::channels() const { return ma_engine_get_channels(&impl_->engine); }

// ---- Mixer -----------------------------------------------------------------

void AudioEngine::set_master_volume(float volume) {
    (void)ma_engine_set_volume(&impl_->engine, sanitize_gain(volume));
}

float AudioEngine::master_volume() const { return ma_engine_get_volume(&impl_->engine); }

void AudioEngine::set_bus_volume(AudioBus bus, float volume) {
    if (bus == AudioBus::master) {
        set_master_volume(volume);
    } else if (ma_sound_group* g = impl_->group(bus)) {
        ma_sound_group_set_volume(g, sanitize_gain(volume));
    }
}

float AudioEngine::bus_volume(AudioBus bus) const {
    if (bus == AudioBus::master) return master_volume();
    const ma_sound_group* g = impl_->group(bus);
    return g != nullptr ? ma_sound_group_get_volume(g) : 0.0F;
}

// ---- Sounds ----------------------------------------------------------------

std::expected<SoundHandle, core::Error> AudioEngine::load(const SoundDesc& desc) {
    Impl& impl = *impl_;
    if (desc.path.empty()) return std::unexpected(core::Error{"audio: empty clip path"});
    if (static_cast<std::size_t>(desc.bus) >= bus_count) {
        return std::unexpected(core::Error{"audio: invalid bus"});
    }
    auto resolved = resolve_clip(impl.asset_root, desc.path, desc.stream,
                                 impl.max_decoded_clip_bytes);
    if (!resolved) return std::unexpected(resolved.error());
    const std::filesystem::path& path = *resolved;

    std::size_t index = impl.slots.size();
    if (!impl.free_slots.empty()) {
        index = impl.free_slots.back();
    } else if (index >= max_sounds) {
        return std::unexpected(core::Error{"audio: too many live sounds"});
    }

    ma_uint32 flags = desc.stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    if (!desc.spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    auto sound = std::make_unique<ma_sound>();
#ifdef _WIN32
    const ma_result r = ma_sound_init_from_file_w(&impl.engine, path.c_str(), flags,
                                                  impl.group(desc.bus), nullptr, sound.get());
#else
    const ma_result r = ma_sound_init_from_file(&impl.engine, path.c_str(), flags,
                                                impl.group(desc.bus), nullptr, sound.get());
#endif
    if (r != MA_SUCCESS) {
        return std::unexpected(ma_error("cannot load '" + desc.path + "'", r));
    }

    if (index == impl.slots.size()) {
        impl.slots.emplace_back();
    } else {
        impl.free_slots.pop_back();
    }
    Impl::Slot& slot = impl.slots[index];
    slot.sound = std::move(sound);
    slot.owner = desc.owner;
    slot.paused = false;
    ++impl.live;
    const SoundHandle handle = make_handle(index, slot.generation);
    if (desc.spatial) set_spatial(handle, SpatialParams{});
    return handle;
}

void AudioEngine::destroy(SoundHandle sound) {
    Impl::Slot* s = impl_->find(sound);
    if (s == nullptr) return;
    impl_->release(*s);
    impl_->free_slots.push_back(static_cast<std::uint16_t>(sound & 0xFFFFU));
}

void AudioEngine::destroy_all() {
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        Impl::Slot& s = impl_->slots[i];
        if (!s.sound) continue;
        impl_->release(s);
        impl_->free_slots.push_back(static_cast<std::uint16_t>(i));
    }
}

bool AudioEngine::valid(SoundHandle sound) const { return impl_->find(sound) != nullptr; }

std::uint32_t AudioEngine::owner(SoundHandle sound) const {
    const Impl::Slot* s = impl_->find(sound);
    return s != nullptr ? s->owner : no_owner;
}

std::size_t AudioEngine::sound_count() const { return impl_->live; }

std::vector<AudioEngine::OwnedSound> AudioEngine::owned_sounds() const {
    std::vector<OwnedSound> out;
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        const Impl::Slot& s = impl_->slots[i];
        if (s.sound && s.owner != no_owner) {
            out.push_back({make_handle(i, s.generation), s.owner});
        }
    }
    return out;
}

void AudioEngine::play(SoundHandle sound) {
    Impl::Slot* s = impl_->find(sound);
    if (s == nullptr) return;
    (void)ma_sound_seek_to_pcm_frame(s->sound.get(), 0);
    (void)ma_sound_start(s->sound.get());
    s->paused = false;
}

void AudioEngine::stop(SoundHandle sound) {
    Impl::Slot* s = impl_->find(sound);
    if (s == nullptr) return;
    (void)ma_sound_stop(s->sound.get());
    (void)ma_sound_seek_to_pcm_frame(s->sound.get(), 0);
    s->paused = false;
}

void AudioEngine::pause(SoundHandle sound) {
    Impl::Slot* s = impl_->find(sound);
    if (s == nullptr || state(sound) != Playback::playing) return;
    (void)ma_sound_stop(s->sound.get()); // keeps the cursor
    s->paused = true;
}

void AudioEngine::resume(SoundHandle sound) {
    Impl::Slot* s = impl_->find(sound);
    if (s == nullptr || !s->paused) return;
    (void)ma_sound_start(s->sound.get());
    s->paused = false;
}

void AudioEngine::stop_all() {
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        if (impl_->slots[i].sound) stop(make_handle(i, impl_->slots[i].generation));
    }
}

Playback AudioEngine::state(SoundHandle sound) const {
    const Impl::Slot* s = impl_->find(sound);
    if (s == nullptr) return Playback::stopped;
    if (s->paused) return Playback::paused;
    // A sound that just hit its end is flagged at_end one mixer step before
    // its node state flips to stopped.
    return ma_sound_is_playing(s->sound.get()) == MA_TRUE &&
                   ma_sound_at_end(s->sound.get()) == MA_FALSE
               ? Playback::playing
               : Playback::stopped;
}

void AudioEngine::set_volume(SoundHandle sound, float volume) {
    if (ma_sound* s = impl_->sound(sound)) ma_sound_set_volume(s, sanitize_gain(volume));
}

void AudioEngine::set_pitch(SoundHandle sound, float pitch) {
    if (ma_sound* s = impl_->sound(sound)) {
        ma_sound_set_pitch(s, std::isfinite(pitch) ? std::clamp(pitch, 1e-3F, 64.0F) : 1.0F);
    }
}

void AudioEngine::set_looping(SoundHandle sound, bool looping) {
    if (ma_sound* s = impl_->sound(sound)) ma_sound_set_looping(s, looping ? MA_TRUE : MA_FALSE);
}

void AudioEngine::set_spatial(SoundHandle sound, const SpatialParams& p) {
    ma_sound* s = impl_->sound(sound);
    if (s == nullptr) return;
    const float min_d = std::isfinite(p.min_distance) ? std::max(p.min_distance, 1e-3F) : 1.0F;
    const float max_d = std::isfinite(p.max_distance) ? std::max(p.max_distance, min_d) : min_d;
    ma_sound_set_attenuation_model(s, ma_attenuation_model_inverse);
    ma_sound_set_min_distance(s, min_d);
    ma_sound_set_max_distance(s, max_d);
    ma_sound_set_rolloff(s, sanitize_gain(p.rolloff));
    ma_sound_set_doppler_factor(s, sanitize_gain(p.doppler));
}

void AudioEngine::set_position(SoundHandle sound, const glm::vec3& p) {
    if (ma_sound* s = impl_->sound(sound)) ma_sound_set_position(s, p.x, p.y, p.z);
}

void AudioEngine::set_velocity(SoundHandle sound, const glm::vec3& v) {
    if (ma_sound* s = impl_->sound(sound)) ma_sound_set_velocity(s, v.x, v.y, v.z);
}

void AudioEngine::set_listener(const ListenerPose& pose) {
    ma_engine* e = &impl_->engine;
    impl_->listener = pose;
    ma_engine_listener_set_position(e, 0, pose.position.x, pose.position.y, pose.position.z);
    // miniaudio orients the listener from its direction plus a world-up
    // vector; feeding the pose's own up keeps a rolled camera correct.
    const glm::vec3 f = glm::length(pose.forward) > 1e-6F ? glm::normalize(pose.forward)
                                                          : glm::vec3(0.0F, 0.0F, -1.0F);
    const glm::vec3 u = glm::length(pose.up) > 1e-6F ? glm::normalize(pose.up)
                                                     : glm::vec3(0.0F, 1.0F, 0.0F);
    ma_engine_listener_set_direction(e, 0, f.x, f.y, f.z);
    ma_engine_listener_set_world_up(e, 0, u.x, u.y, u.z);
    ma_engine_listener_set_velocity(e, 0, pose.velocity.x, pose.velocity.y, pose.velocity.z);
}

ListenerPose AudioEngine::listener() const { return impl_->listener; }

// ---- Headless mixing -------------------------------------------------------

std::expected<void, core::Error> AudioEngine::read_frames(std::span<float> out) {
    if (impl_->has_device) {
        return std::unexpected(core::Error{"audio: read_frames needs a no_device engine"});
    }
    const std::uint32_t ch = channels();
    if (ch == 0 || out.size() % ch != 0) {
        return std::unexpected(core::Error{"audio: buffer is not a whole number of frames"});
    }
    const ma_uint64 frames = out.size() / ch;
    ma_uint64 read = 0;
    if (const ma_result r = ma_engine_read_pcm_frames(&impl_->engine, out.data(), frames, &read);
        r != MA_SUCCESS && r != MA_AT_END) {
        return std::unexpected(ma_error("mix failed", r));
    }
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(read * ch), out.end(), 0.0F);
    return {};
}

} // namespace engine::audio
