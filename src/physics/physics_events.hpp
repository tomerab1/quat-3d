#pragma once

// Contact / trigger event collection for PhysicsWorld (ROADMAP A10.1).
// Private to engine_physics; included by physics_world_impl.hpp after the
// Jolt headers.
//
// Jolt calls the contact listener from its worker threads during
// PhysicsSystem::Update, once per touching sub-shape pair. The listener only
// appends a RawContact to the current step's ContactBuffer: a fixed array
// claimed with one atomic fetch_add (no lock; full = counted as dropped).
// Two buffers alternate: after each step the main thread flips the write
// index and hands the finished buffer to ContactTracker, which turns the raw
// sub-shape callbacks into one event per body pair (contact_added when two
// bodies start touching, contact_removed when they stop, or one is removed;
// a body falling asleep — Jolt then reports its contacts as removed — does
// not end them).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/physics/physics_world.hpp"

namespace JPH {
class BodyInterface;
}

namespace engine::physics::detail {

struct RawContact {
    std::uint32_t body1 = PhysicsWorld::invalid_body;
    std::uint32_t body2 = PhysicsWorld::invalid_body;
    glm::vec3     point{0.0F};
    glm::vec3     normal{0.0F}; // from body1 towards body2
    float         impulse = 0.0F;
    bool          added = true;  // false: OnContactRemoved
    bool          trigger = false;
    bool          sensor1 = false; // body1 is the sensor (trigger contacts)
};

// One step's raw contacts. Written concurrently by Jolt's workers (push),
// read by the main thread once the step has returned.
class ContactBuffer {
public:
    explicit ContactBuffer(std::size_t capacity = PhysicsWorld::max_contacts_per_step)
        : slots_(capacity) {}

    void push(const RawContact& c) {
        const std::uint32_t i = count_.fetch_add(1, std::memory_order_relaxed);
        if (i < slots_.size()) {
            slots_[i] = c;
        } else {
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] std::span<const RawContact> contacts() const {
        const std::size_t n = count_.load(std::memory_order_relaxed);
        return {slots_.data(), n < slots_.size() ? n : slots_.size()};
    }
    [[nodiscard]] std::uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void clear() {
        count_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<RawContact>    slots_;
    std::atomic<std::uint32_t> count_{0};
    std::atomic<std::uint32_t> dropped_{0};
};

// Double-buffered per-step contact collection. The listener writes into
// `write()`; the main thread calls swap() after each step and reads the
// returned buffer while the next step writes into the other one.
class ContactBuffers {
public:
    [[nodiscard]] ContactBuffer& write() {
        return buffers_[write_index_.load(std::memory_order_acquire)];
    }
    // Makes the other buffer current (cleared) and returns the finished one.
    [[nodiscard]] ContactBuffer& swap() {
        const unsigned done = write_index_.load(std::memory_order_relaxed);
        buffers_[done ^ 1U].clear();
        write_index_.store(done ^ 1U, std::memory_order_release);
        return buffers_[done];
    }

private:
    std::array<ContactBuffer, 2> buffers_;
    std::atomic<unsigned>        write_index_{0};
};

// Turns per-sub-shape callbacks into per-body-pair events (main thread).
class ContactTracker {
public:
    // Processes one step's raw contacts (adds first, then removals) and
    // appends the resulting events to `out`. `world` maps bodies to owners,
    // `bodies` tells which still exist and which are awake.
    void process(std::span<const RawContact> raw, std::uint64_t step, const PhysicsWorld& world,
                 const JPH::BodyInterface& bodies, std::vector<PhysicsWorld::ContactEvent>& out);

    // `body` is being removed: its sleeping (dormant) contacts end now,
    // since Jolt reports only awake contacts of removed bodies.
    void body_removed(std::uint32_t body, std::uint64_t step,
                      std::vector<PhysicsWorld::ContactEvent>& out);

    [[nodiscard]] std::size_t pair_count() const { return pairs_.size(); }

private:
    struct Pair {
        std::uint32_t a = PhysicsWorld::invalid_body; // as reported (sensor first)
        std::uint32_t b = PhysicsWorld::invalid_body;
        std::uint32_t owner_a = PhysicsWorld::no_owner;
        std::uint32_t owner_b = PhysicsWorld::no_owner;
        std::uint32_t touching = 0; // sub-shape contacts currently reported
        bool          trigger = false;
        bool          dormant = false; // touching, but a body fell asleep
        bool          added_now = false;
        // Index into `out` of the begin event while the pair began in the
        // step being processed (no_event otherwise): later sub-shape adds of
        // that step update it directly instead of searching.
        std::size_t   begin_event = no_event;
    };
    static constexpr std::size_t no_event = static_cast<std::size_t>(-1);
    [[nodiscard]] static std::uint64_t key(std::uint32_t a, std::uint32_t b) {
        return a < b ? (std::uint64_t{a} << 32U) | b : (std::uint64_t{b} << 32U) | a;
    }
    static void emit_end(const Pair& p, std::uint64_t step,
                         std::vector<PhysicsWorld::ContactEvent>& out);

    std::unordered_map<std::uint64_t, Pair> pairs_;
    std::size_t                             dormant_ = 0; // pairs with `dormant` set
};

} // namespace engine::physics::detail
