#pragma once

// Client-side prediction + reconciliation (Phase 14, Slice 14.3).
//
// The owning client applies its character inputs immediately (prediction) and
// remembers them with sequence numbers. The server simulates the same inputs
// authoritatively and acknowledges the last sequence it processed alongside
// the resulting state. When an authoritative state arrives, the client drops
// acknowledged inputs and — if its prediction diverged — rewinds to the server
// state and replays the still-unacknowledged inputs through the same step
// function, so corrections never erase the player's recent input.
//
// CharacterPredictor is engine-agnostic: the step function is injected, which
// keeps the replay math unit-testable without physics or sockets.

#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::net {

struct PlayerInput {
    std::uint32_t sequence = 0;
    glm::vec3     move{0.0F}; // desired horizontal direction, |xz| <= 1
    float         dt = 0.0F;
};

// Wire helpers (kind 2 = input, kind 3 = authoritative character state).
[[nodiscard]] std::vector<std::uint8_t> encode_input(const PlayerInput& input);
[[nodiscard]] bool decode_input(const std::vector<std::uint8_t>& payload, PlayerInput& out);
[[nodiscard]] std::vector<std::uint8_t> encode_character_state(std::uint32_t acked_sequence,
                                                               const glm::vec3& position);
[[nodiscard]] bool decode_character_state(const std::vector<std::uint8_t>& payload,
                                          std::uint32_t& acked_sequence, glm::vec3& position);

class CharacterPredictor {
public:
    // `step` advances a position by one input (the same rule the server runs).
    using StepFn = std::function<glm::vec3(const glm::vec3& position, const PlayerInput&)>;

    explicit CharacterPredictor(StepFn step) : step_(std::move(step)) {}
    CharacterPredictor() = default;

    // Record + apply one local input; returns the predicted position.
    glm::vec3 predict(const glm::vec3& current, const PlayerInput& input);

    // Authoritative state for `acked_sequence` arrived: drop acknowledged
    // inputs and re-derive the local position by replaying the rest on top of
    // the server position. Returns the corrected position (equal to the
    // prediction when nothing diverged).
    glm::vec3 reconcile(std::uint32_t acked_sequence, const glm::vec3& server_position);

    [[nodiscard]] std::size_t pending() const { return pending_.size(); }

private:
    StepFn                  step_;
    std::deque<PlayerInput> pending_;
};

// Pure-CPU self-test of the replay math: a deliberate server correction mid-
// stream must land the client exactly on server-state + unacked inputs.
[[nodiscard]] std::expected<void, core::Error> run_prediction_self_test();

} // namespace engine::net
