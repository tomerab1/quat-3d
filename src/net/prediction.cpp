#include "engine/net/prediction.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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

void InputQueue::reset() {
    queue_.clear();
    last_received_ = 0;
}

std::vector<std::uint8_t> encode_character_state(std::uint32_t acked_sequence,
                                                 const glm::vec3& position) {
    std::vector<std::uint8_t> out(1 + 4 + 12);
    out[0] = kStateKind;
    std::memcpy(out.data() + 1, &acked_sequence, 4);
    std::memcpy(out.data() + 5, &position, 12);
    return out;
}

bool decode_character_state(const std::vector<std::uint8_t>& payload,
                            std::uint32_t& acked_sequence, glm::vec3& position) {
    if (payload.size() != 17 || payload[0] != kStateKind) return false;
    std::memcpy(&acked_sequence, payload.data() + 1, 4);
    std::memcpy(&position, payload.data() + 5, 12);
    return true;
}

glm::vec3 CharacterPredictor::predict(const glm::vec3& current, const PlayerInput& input) {
    pending_.push_back(input);
    return step_ ? step_(current, input) : current;
}

glm::vec3 CharacterPredictor::reconcile(std::uint32_t acked_sequence,
                                        const glm::vec3& server_position) {
    while (!pending_.empty() && pending_.front().sequence <= acked_sequence) {
        pending_.pop_front();
    }
    glm::vec3 position = server_position;
    if (step_) {
        for (const PlayerInput& input : pending_) {
            position = step_(position, input);
        }
    }
    return position;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
std::expected<void, core::Error> run_prediction_self_test() {
    const auto fail = [](const char* what) {
        return std::unexpected(core::Error{std::string(what)});
    };

    // Deterministic toy locomotion: position += move * 2 m/s * dt.
    const auto step = [](const glm::vec3& p, const PlayerInput& in) {
        return p + in.move * (2.0F * in.dt);
    };
    CharacterPredictor predictor{CharacterPredictor::StepFn(step)};

    // Wire round trip.
    PlayerInput probe{41U, glm::vec3(0.5F, 0.0F, -1.0F), 1.0F / 60.0F};
    PlayerInput decoded;
    if (!decode_input(encode_input(probe), decoded) || decoded.sequence != 41U ||
        decoded.move != probe.move || decoded.dt != probe.dt) {
        return fail("prediction self-test: input wire round trip failed");
    }

    // Predict 10 inputs of +x movement.
    glm::vec3 client(0.0F);
    glm::vec3 server(0.0F);
    std::vector<PlayerInput> inputs;
    for (std::uint32_t s = 1; s <= 10; ++s) {
        PlayerInput in{s, glm::vec3(1.0F, 0.0F, 0.0F), 0.1F};
        inputs.push_back(in);
        client = predictor.predict(client, in);
    }
    // The server processed inputs 1..5 but ended somewhere slightly different
    // (e.g. it collided with something the client did not predict).
    for (std::uint32_t s = 0; s < 5; ++s) server = step(server, inputs[s]);
    server += glm::vec3(0.0F, 0.0F, 0.4F); // authoritative divergence

    const glm::vec3 corrected = predictor.reconcile(5U, server);

    // Expected: server state + replay of inputs 6..10.
    glm::vec3 expected = server;
    for (std::uint32_t s = 5; s < 10; ++s) expected = step(expected, inputs[s]);
    if (glm::distance(corrected, expected) > 1e-5F) {
        return fail("prediction self-test: replay did not land on server + unacked inputs");
    }
    if (predictor.pending() != 5) {
        return fail("prediction self-test: acked inputs were not dropped");
    }
    // A second state for the final sequence clears the queue entirely.
    (void)predictor.reconcile(10U, expected);
    if (predictor.pending() != 0) {
        return fail("prediction self-test: queue not empty after full ack");
    }
    return {};
}

} // namespace engine::net
