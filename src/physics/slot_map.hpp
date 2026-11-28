#pragma once

// SlotMap — generational slots behind PhysicsWorld's integer handles (shapes,
// characters, joints). Private to engine_physics; tests include it directly.
//
// A handle is (generation << 24) | index. Freed indices go on a free list and
// are reused, so long sessions that create and destroy objects keep a bounded
// table (issue #47); the generation, bumped on every free, makes a stale
// handle to a recycled slot resolve to nothing instead of aliasing the new
// occupant. The generation is 8 bits; a slot whose generation would wrap is
// retired rather than reused, so no handle ever comes back to life. Index 0xFFFFFF is never handed out, so 0xFFFFFFFF (the engine's
// "invalid" sentinel) is never a live handle.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine::physics::detail {

template <typename T>
class SlotMap {
public:
    static constexpr std::uint32_t invalid = 0xFFFFFFFFU;
    static constexpr std::uint32_t index_bits = 24;
    static constexpr std::uint32_t index_mask = (1U << index_bits) - 1U;
    static constexpr std::uint32_t max_slots = index_mask; // index_mask itself is reserved

    // Stores `value` and returns its handle (invalid when every slot is used).
    [[nodiscard]] std::uint32_t insert(T value) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            if (slots_.size() >= max_slots) return invalid;
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& s = slots_[index];
        s.value = std::move(value);
        s.live = true;
        ++live_;
        return handle(index, s.generation);
    }

    [[nodiscard]] T* get(std::uint32_t id) {
        Slot* s = find(id);
        return s != nullptr ? &s->value : nullptr;
    }
    [[nodiscard]] const T* get(std::uint32_t id) const {
        const Slot* s = const_cast<SlotMap*>(this)->find(id);
        return s != nullptr ? &s->value : nullptr;
    }
    [[nodiscard]] bool contains(std::uint32_t id) const { return get(id) != nullptr; }

    // Frees the slot (its value is reset, releasing what it holds). False for
    // a stale or invalid handle.
    bool erase(std::uint32_t id) {
        Slot* s = find(id);
        if (s == nullptr) return false;
        s->value = T{};
        s->live = false;
        s->generation = static_cast<std::uint8_t>(s->generation + 1U);
        // A wrapped generation would make the slot's first handle valid again
        // (an alias of whatever lives there next). Retire the index instead:
        // one slot per 256 reuses of it, a bounded and negligible cost.
        if (s->generation != 0) {
            free_.push_back(id & index_mask);
        } else {
            ++retired_;
        }
        --live_;
        return true;
    }

    // Live entries.
    [[nodiscard]] std::size_t size() const { return live_; }
    // Slots ever allocated (live + free + retired): what the table costs in memory.
    [[nodiscard]] std::size_t capacity() const { return slots_.size(); }
    // Slots retired after their generation wrapped (never reused).
    [[nodiscard]] std::size_t retired() const { return retired_; }

    // f(handle, value&) for every live entry, in index order.
    template <typename F>
    void for_each(F&& f) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot& s = slots_[i];
            if (s.live) f(handle(static_cast<std::uint32_t>(i), s.generation), s.value);
        }
    }
    template <typename F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            if (s.live) f(handle(static_cast<std::uint32_t>(i), s.generation), s.value);
        }
    }

private:
    struct Slot {
        T            value{};
        std::uint8_t generation = 0;
        bool         live = false;
    };

    [[nodiscard]] static std::uint32_t handle(std::uint32_t index, std::uint8_t generation) {
        return (static_cast<std::uint32_t>(generation) << index_bits) | index;
    }
    [[nodiscard]] Slot* find(std::uint32_t id) {
        if (id == invalid) return nullptr;
        const std::uint32_t index = id & index_mask;
        if (index >= slots_.size()) return nullptr;
        Slot& s = slots_[index];
        if (!s.live || s.generation != static_cast<std::uint8_t>(id >> index_bits)) return nullptr;
        return &s;
    }

    std::vector<Slot>          slots_;
    std::vector<std::uint32_t> free_;
    std::size_t                live_ = 0;
    std::size_t                retired_ = 0;
};

} // namespace engine::physics::detail
