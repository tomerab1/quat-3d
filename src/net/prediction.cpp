#include "engine/net/prediction.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::net {

namespace {
constexpr std::uint8_t kInputKind = 2;
constexpr std::uint8_t kStateKind = 3;
} // namespace

std::vector<std::uint8_t> encode_input(const PlayerInput& input) {
    std::vector<std::uint8_t> out(1 + 4 + 12 + 4);
    out[0] = kInputKind;
    std::memcpy(out.data() + 1, &input.sequence, 4);
    std::memcpy(out.data() + 5, &input.move, 12);
    std::memcpy(out.data() + 17, &input.dt, 4);
    return out;
}

bool decode_input(const std::vector<std::uint8_t>& payload, PlayerInput& out) {
    if (payload.size() != 21 || payload[0] != kInputKind) return false;
    PlayerInput in;
    std::memcpy(&in.sequence, payload.data() + 1, 4);
    std::memcpy(&in.move, payload.data() + 5, 12);
    std::memcpy(&in.dt, payload.data() + 17, 4);
    if (!sanitize_input(in)) return false;
    out = in;
    return true;
}

bool sanitize_input(PlayerInput& input) {
    const glm::vec3& m = input.move;
    if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.z) ||
        !std::isfinite(input.dt) || !(input.dt > 0.0F)) {
        return false;
    }
    // Horizontal only, |xz| <= 1. Pre-scale by the largest component so huge
    // (finite) values cannot overflow the length to infinity.
    glm::vec2 xz(m.x, m.z);
    const float largest = std::max(std::abs(xz.x), std::abs(xz.y));
    if (largest > 1.0F) xz /= largest;
    const float len = glm::length(xz);
    if (len > 1.0F) xz /= len;
    input.move = glm::vec3(xz.x, 0.0F, xz.y);
    input.dt = std::min(input.dt, max_input_dt);
    return true;
}

std::optional<PlayerInput> make_input(std::uint32_t sequence, const glm::vec3& move, float dt) {
    PlayerInput input{sequence, move, dt};
    if (!sanitize_input(input)) return std::nullopt;
    return input;
}

InputQueue::Push InputQueue::push(PlayerInput input) {
    if (!sanitize_input(input)) return Push::malformed;
    if (input.sequence <= last_received_) return Push::stale;
    if (queue_.size() >= capacity) return Push::full;
    last_received_ = input.sequence;
    queue_.push_back(input);
    return Push::queued;
}

void InputQueue::credit(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0F) return;
    budget_ = std::min(budget_ + dt, max_time_budget);
}

std::optional<PlayerInput> InputQueue::take() {
    // A little tolerance so a client running at exactly the host's rate is
    // not held back a frame by float rounding.
    constexpr float tolerance = 1e-5F;
    if (queue_.empty() || queue_.front().dt > budget_ + tolerance) return std::nullopt;
    const PlayerInput input = queue_.front();
    queue_.pop_front();
    budget_ = std::max(budget_ - input.dt, 0.0F);
    acked_ = input.sequence;
    return input;
}

void InputQueue::reset() {
    queue_.clear();
    last_received_ = 0;
    acked_ = 0;
    budget_ = 0.0F;
}

std::vector<std::uint8_t> encode_character_state(std::uint32_t acked_sequence,
                                                 const CharacterState& state) {
    std::vector<std::uint8_t> out(1 + 4 + 12 + 12 + 1);
    out[0] = kStateKind;
    std::memcpy(out.data() + 1, &acked_sequence, 4);
    std::memcpy(out.data() + 5, &state.position, 12);
    std::memcpy(out.data() + 17, &state.velocity, 12);
    out[29] = state.on_ground ? 1 : 0;
    return out;
}

bool decode_character_state(const std::vector<std::uint8_t>& payload,
                            std::uint32_t& acked_sequence, CharacterState& state) {
    if (payload.size() != 30 || payload[0] != kStateKind || payload[29] > 1) return false;
    CharacterState s;
    std::uint32_t acked = 0;
    std::memcpy(&acked, payload.data() + 1, 4);
    std::memcpy(&s.position, payload.data() + 5, 12);
    std::memcpy(&s.velocity, payload.data() + 17, 12);
    s.on_ground = payload[29] == 1;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(s.position[i]) || !std::isfinite(s.velocity[i])) return false;
    }
    acked_sequence = acked;
    state = s;
    return true;
}

namespace {

[[nodiscard]] bool close_enough(const CharacterState& a, const CharacterState& b) {
    return a.on_ground == b.on_ground &&
           glm::distance(a.position, b.position) <= CharacterPredictor::position_tolerance &&
           glm::distance(a.velocity, b.velocity) <= CharacterPredictor::velocity_tolerance;
}

} // namespace

CharacterState CharacterPredictor::predict(const CharacterState& current, const PlayerInput& input,
                                           const StepFn& step) {
    if (pending_.size() >= max_pending) {
        pending_.pop_front();
        ++dropped_;
    }
    const CharacterState next = step ? step(current, input) : current;
    pending_.push_back({input, next});
    return next;
}

std::optional<CharacterState> CharacterPredictor::reconcile(std::uint32_t acked_sequence,
                                                            const CharacterState& server,
                                                            const StepFn& step) {
    if (acked_sequence <= last_acked_) return std::nullopt; // stale / reordered
    last_acked_ = acked_sequence;

    while (!pending_.empty() && pending_.front().input.sequence < acked_sequence) {
        pending_.pop_front();
    }
    // Compare against what this client predicted for the acknowledged input.
    // If that entry is gone (dropped by the cap), the server state cannot be
    // checked and is taken as a correction.
    if (!pending_.empty() && pending_.front().input.sequence == acked_sequence) {
        const bool held = close_enough(pending_.front().predicted, server);
        pending_.pop_front();
        if (held) return std::nullopt;
    }

    ++corrections_;
    CharacterState state = server;
    for (Pending& p : pending_) {
        if (step) state = step(state, p.input);
        p.predicted = state;
    }
    return state;
}

void CharacterPredictor::reset() {
    pending_.clear();
    last_acked_ = 0;
}

} // namespace engine::net
