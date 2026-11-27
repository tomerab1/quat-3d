#include "engine/net/session.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <entt/entity/registry.hpp>

#include "engine/net/character_step.hpp"
#include "engine/net/net_ids.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::net {

namespace {

[[nodiscard]] const char* push_result_name(InputQueue::Push p) {
    switch (p) {
    case InputQueue::Push::queued:
        return "queued";
    case InputQueue::Push::malformed:
        return "malformed";
    case InputQueue::Push::stale:
        return "stale sequence";
    case InputQueue::Push::full:
        return "queue full";
    }
    return "?";
}

[[nodiscard]] bool controllable(const entt::registry& registry, entt::entity character) {
    return character != entt::null && registry.valid(character) &&
           registry.all_of<scene::CharacterController, scene::Transform>(character);
}

// Host and client step the character with the same rule; see character_step.hpp.
[[nodiscard]] CharacterPredictor::StepFn character_stepper(entt::registry& registry,
                                                           physics::PhysicsWorld& world,
                                                           entt::entity character) {
    return [&registry, &world, character](const CharacterState& s, const PlayerInput& in) {
        return step_character_input(registry, world, character, s, in);
    };
}

} // namespace

NetSession::NetSession() = default;
NetSession::NetSession(NetSession&&) noexcept = default;
NetSession& NetSession::operator=(NetSession&&) noexcept = default;
NetSession::~NetSession() = default;

NetSession NetSession::from_environment(entt::registry& registry) {
    NetSession session;
    if (const char* host_port = std::getenv("QUAT_HOST"); host_port != nullptr) {
        if (auto s = NetServer::listen(static_cast<std::uint16_t>(std::atoi(host_port))); s) {
            session.server_ = std::move(*s);
            (void)assign_net_ids(registry);
            std::fprintf(stderr, "[net] hosting on port %s\n", host_port);
        } else {
            std::fprintf(stderr, "[net] host failed: %s\n", s.error().message.c_str());
        }
    } else if (const char* join_addr = std::getenv("QUAT_JOIN"); join_addr != nullptr) {
        const std::string addr(join_addr);
        const std::size_t colon = addr.rfind(':');
        if (colon == std::string::npos) {
            std::fprintf(stderr, "[net] QUAT_JOIN must be <ip:port>, got '%s'\n", join_addr);
        } else if (auto c = NetClient::connect(
                       addr.substr(0, colon),
                       static_cast<std::uint16_t>(std::atoi(addr.c_str() + colon + 1)));
                   c) {
            session.client_ = std::move(*c);
            (void)assign_net_ids(registry);
            // The host simulates; local copies of its bodies only follow.
            const std::uint32_t replicas = make_replicas_kinematic(registry);
            std::fprintf(stderr, "[net] joining %s (%u replicated bodies made kinematic)\n",
                         join_addr, replicas);
        } else {
            std::fprintf(stderr, "[net] join failed: %s\n", c.error().message.c_str());
        }
    }
    return session;
}

void NetSession::drive_character(entt::registry& registry, physics::PhysicsWorld* world,
                                 entt::entity character, const glm::vec3& move, float dt) {
    if (!controllable(registry, character)) return;
    auto& cc = registry.get<scene::CharacterController>(character);

    if (server_ && owner_ != invalid_client) return; // the remote player drives it
    if (!client_ || !client_->connected()) {
        cc.move = move; // offline / host's own character / not connected yet
        return;
    }

    // Joined: ship the input and predict it locally with the host's step; the
    // host is authoritative. The character is stepped here (and replayed in
    // reconciliation), never by character_system.
    if (!registry.all_of<scene::NetDriven>(character)) {
        registry.emplace<scene::NetDriven>(character);
    }
    const std::optional<PlayerInput> input = make_input(next_sequence_ + 1, move, dt);
    if (!input) return;
    next_sequence_ = input->sequence;
    if (!client_->send(encode_input(*input), /*reliable=*/true)) {
        std::fprintf(stderr, "[net] input %u refused by the transport\n", input->sequence);
    }
    if (world == nullptr) return; // no physics: the host's states place the character
    (void)predictor_.predict(character_state(registry, character), *input,
                             character_stepper(registry, *world, character));
}

void NetSession::update(entt::registry& registry, physics::PhysicsWorld* world,
                        entt::entity character, float dt) {
    if (server_) {
        host_update(registry, world, character, dt);
    } else if (client_) {
        client_update(registry, world, character, dt);
    }
}

void NetSession::publish(entt::registry& registry, float dt) {
    if (server_) replication_.tick(registry, *server_, dt);
}

void NetSession::release_character(entt::registry& registry, entt::entity character) {
    owner_ = invalid_client;
    inputs_.reset();
    if (character != entt::null && registry.valid(character)) {
        registry.remove<scene::NetDriven>(character);
        if (auto* cc = registry.try_get<scene::CharacterController>(character)) {
            cc->move = glm::vec3(0.0F);
        }
    }
}

void NetSession::host_update(entt::registry& registry, physics::PhysicsWorld* world,
                             entt::entity character, float dt) {
    for (const NetEvent& ev : server_->poll()) {
        if (ev.type == NetEvent::Type::connected) {
            std::fprintf(stderr, "[net] client %u connected\n", ev.client);
            continue;
        }
        if (ev.type == NetEvent::Type::disconnected) {
            std::fprintf(stderr, "[net] client %u left\n", ev.client);
            if (ev.client == owner_) release_character(registry, character);
            continue;
        }
        PlayerInput input;
        if (!decode_input(ev.payload, input)) {
            if (rejected_inputs_++ % 100 == 0) {
                std::fprintf(stderr, "[net] dropped malformed message from client %u\n",
                             ev.client);
            }
            continue;
        }
        // First sender claims the player character (v1: one remote player).
        if (owner_ == invalid_client && controllable(registry, character)) {
            owner_ = ev.client;
            inputs_.reset();
            registry.emplace_or_replace<scene::NetDriven>(character);
            std::fprintf(stderr, "[net] client %u controls the player character\n", ev.client);
        }
        if (ev.client != owner_) continue;
        if (const InputQueue::Push r = inputs_.push(input); r != InputQueue::Push::queued) {
            if (rejected_inputs_++ % 100 == 0) {
                std::fprintf(stderr, "[net] input %u from client %u rejected: %s\n",
                             input.sequence, ev.client, push_result_name(r));
            }
        }
    }

    if (owner_ == invalid_client) return;
    if (!controllable(registry, character)) {
        // The character was deleted under the remote player.
        release_character(registry, character);
        return;
    }
    // Simulate the remote inputs due this frame, one character step each (the
    // same step the client predicts with), then acknowledge what ran.
    inputs_.credit(dt);
    if (world == nullptr) return;
    const CharacterPredictor::StepFn step = character_stepper(registry, *world, character);
    std::optional<CharacterState> state;
    while (const std::optional<PlayerInput> input = inputs_.take()) {
        state = step(character_state(registry, character), *input);
    }
    if (!state) return;
    // Acknowledge exactly what was simulated, with the state it produced.
    // Unreliable latest-wins: a refused state is superseded by the next one.
    (void)server_->send(owner_, encode_character_state(inputs_.acked(), *state),
                        /*reliable=*/false);
}

void NetSession::client_update(entt::registry& registry, physics::PhysicsWorld* world,
                               entt::entity character, float dt) {
    // Only the newest authoritative state of this poll matters.
    std::optional<std::pair<std::uint32_t, CharacterState>> latest;
    for (const NetEvent& ev : client_->poll()) {
        if (ev.type == NetEvent::Type::connected) {
            std::fprintf(stderr, "[net] connected to host\n");
            continue;
        }
        if (ev.type == NetEvent::Type::disconnected) {
            std::fprintf(stderr, "[net] disconnected from host\n");
            predictor_.reset();
            if (character != entt::null && registry.valid(character)) {
                registry.remove<scene::NetDriven>(character); // local simulation resumes
            }
            continue;
        }
        std::uint32_t acked = 0;
        CharacterState server;
        if (decode_character_state(ev.payload, acked, server)) {
            if (!latest || acked > latest->first) latest.emplace(acked, server);
        } else {
            receiver_.receive(ev.payload);
        }
    }

    if (latest && controllable(registry, character)) {
        if (world == nullptr) {
            // No local physics to predict with: show the host's state.
            auto& tf = registry.get<scene::Transform>(character);
            tf.local[3] = glm::vec4(latest->second.position, 1.0F);
            tf.world[3] = glm::vec4(latest->second.position, 1.0F);
        } else if (const std::optional<CharacterState> corrected = predictor_.reconcile(
                       latest->first, latest->second,
                       character_stepper(registry, *world, character))) {
            // Rewound + replayed through the CharacterVirtual; with nothing
            // left to replay it still has to be moved onto the server state.
            restore_character(registry, *world, character, *corrected);
        }
    }
    receiver_.apply(registry, dt);
}

} // namespace engine::net
