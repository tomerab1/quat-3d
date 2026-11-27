#pragma once

// ParticleEmitterTable — the CPU bookkeeping between a scene's emitters and
// ParticlePass (ROADMAP R10.3): each emitter, keyed by a stable id (the app
// uses the entity), owns one of the particle_max_emitters slots (its counters
// and indirect arguments) and a contiguous range of the global pool sized by
// its max_particles, plus its emission schedule.
//
// Per frame:
//   begin_frame();
//   for each emitter: if (auto info = visit(key, settings, world, dt)) ...;
//   end_frame();                 // frees emitters not visited this frame
//   ...ParticlePass::add_to_graph(...) with the infos...
//   commit();                    // only once the frame was recorded
// An emitter is (re)placed — new slot and range, `reset` set so the pass
// reinitialises the range, emission restarted — when first seen, when its
// max_particles or space changes, and after reset_all(). Until commit() its
// reset stays pending, so a frame that failed to record resets it again.

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/renderer/particles.hpp"

namespace engine::renderer {

class ParticleEmitterTable {
public:
    struct Entry {
        std::uint32_t      slot = 0;
        std::uint32_t      base = 0;
        std::uint32_t      capacity = 0;
        ParticleSpace      space = ParticleSpace::world;
        ParticleSpawnState spawn{};
        bool               placed = false; // false: waiting for budget
        bool               fresh = true;   // reset pending (not yet committed)
        bool               visited = false;
    };

    explicit ParticleEmitterTable(std::uint32_t pool_capacity = particle_default_pool);

    void begin_frame();

    // This frame's input of emitter `key` with sanitized `settings` at
    // `world`, advancing its emission schedule by `dt` (0 = paused). Places
    // or re-places it as described above; nullopt while the pool or the
    // slots are exhausted (it is retried on the next visit).
    [[nodiscard]] std::optional<EmitterFrameInfo>
    visit(std::uint64_t key, const ParticleEmitterSettings& settings, const glm::mat4& world,
          float dt);

    // Frees the slot and range of every emitter not visited since
    // begin_frame(); returns how many were removed. An emitter waiting for
    // budget gets the freed space on its next visit (one frame later).
    std::size_t end_frame();

    // The frame's inputs were recorded: pending resets are done.
    void commit();

    // Forgets every emitter (Play / Stop, scene restore): each is placed
    // anew on its next visit — range reinitialised, emission restarted.
    void reset_all();

    [[nodiscard]] const Entry* find(std::uint64_t key) const;
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] std::uint32_t pool_capacity() const { return pool_.capacity(); }
    [[nodiscard]] std::uint32_t free_particles() const { return pool_.free_total(); }
    [[nodiscard]] std::uint32_t free_slots() const;

private:
    [[nodiscard]] bool place(Entry& entry, const ParticleEmitterSettings& settings);
    void release(Entry& entry);

    ParticlePoolAllocator                       pool_;
    std::array<bool, particle_max_emitters>     slot_used_{};
    std::unordered_map<std::uint64_t, Entry>    entries_;
};

} // namespace engine::renderer
