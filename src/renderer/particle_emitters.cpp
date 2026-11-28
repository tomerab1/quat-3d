#include "engine/renderer/particle_emitters.hpp"

#include <algorithm>

namespace engine::renderer {

ParticleEmitterTable::ParticleEmitterTable(std::uint32_t pool_capacity) : pool_(pool_capacity) {}

void ParticleEmitterTable::begin_frame() {
    for (auto& [key, e] : entries_) e.visited = false;
}

void ParticleEmitterTable::release(Entry& e) {
    if (!e.placed) return;
    pool_.free(e.base, e.capacity);
    slot_used_[e.slot] = false;
    e.placed = false;
    e.capacity = 0;
}

bool ParticleEmitterTable::place(Entry& e, const ParticleEmitterSettings& settings) {
    release(e);
    const auto free_slot = std::find(slot_used_.begin(), slot_used_.end(), false);
    if (free_slot == slot_used_.end()) return false;
    const std::uint32_t base = pool_.allocate(settings.max_particles);
    if (base == UINT32_MAX) return false;
    *free_slot = true;
    e.slot = static_cast<std::uint32_t>(free_slot - slot_used_.begin());
    e.base = base;
    e.capacity = settings.max_particles;
    e.space = settings.space;
    e.spawn = {};
    e.fresh = true;
    e.placed = true;
    return true;
}

std::optional<EmitterFrameInfo> ParticleEmitterTable::visit(std::uint64_t key,
                                                            const ParticleEmitterSettings& settings,
                                                            const glm::mat4& world, float dt) {
    Entry& e = entries_[key];
    e.visited = true;
    if (!e.placed || e.capacity != settings.max_particles || e.space != settings.space) {
        if (!place(e, settings)) return std::nullopt;
    }
    EmitterFrameInfo info;
    info.world = world;
    info.slot = e.slot;
    info.pool_base = e.base;
    info.spawn_index = e.spawn.spawned;
    info.spawn_count = advance_spawn(e.spawn, settings, dt);
    info.dt = dt;
    info.reset = e.fresh;
    return info;
}

std::size_t ParticleEmitterTable::end_frame() {
    return std::erase_if(entries_, [this](auto& item) {
        if (item.second.visited) return false;
        release(item.second);
        return true;
    });
}

void ParticleEmitterTable::commit() {
    for (auto& [key, e] : entries_) {
        if (e.placed && e.visited) e.fresh = false;
    }
}

void ParticleEmitterTable::reset_all() {
    for (auto& [key, e] : entries_) release(e);
    entries_.clear();
}

const ParticleEmitterTable::Entry* ParticleEmitterTable::find(std::uint64_t key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

std::uint32_t ParticleEmitterTable::free_slots() const {
    return static_cast<std::uint32_t>(std::count(slot_used_.begin(), slot_used_.end(), false));
}

} // namespace engine::renderer
