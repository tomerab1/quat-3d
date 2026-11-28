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
    const auto config =
        session_config_from_env([](const char* name) { return std::getenv(name); });
    if (!config) {
        std::fprintf(stderr, "[net] not starting: %s\n", config.error().message.c_str());
        return {};
    }
    if (config->host) {
        auto s = host(registry, *config->host);
        if (s) return std::move(*s);
        std::fprintf(stderr, "[net] host failed: %s\n", s.error().message.c_str());
    } else if (config->join) {
        auto c = join(registry, *config->join);
        if (c) return std::move(*c);
        std::fprintf(stderr, "[net] join failed: %s\n", c.error().message.c_str());
    }
    return {};
}

std::expected<NetSession, core::Error> NetSession::host(entt::registry& registry,
                                                        const HostConfig& config) {
    if (auto ok = validate(config.access); !ok) return std::unexpected(ok.error());
    auto server = NetServer::listen(config.server);
    if (!server) return std::unexpected(server.error());

    NetSession session;
    session.server_ = std::move(*server);
    session.access_ = HostAccess(config.access);
    (void)assign_net_ids(registry);

    // listen() accepted the address, so it parses; 127.0.0.0/8 is loopback.
    const bool loopback = (parse_ipv4(config.server.bind).value_or(0U) >> 24U) == 127U;
    std::fprintf(stderr, "[net] hosting on %s:%u (%s, up to %u connections, %s%s)\n",
                 config.server.bind.c_str(), static_cast<unsigned>(config.server.port),
                 loopback ? "this machine only" : "reachable from the network",
                 config.server.max_clients,
                 config.access.join_token.empty() ? "no join token" : "join token required",
                 config.access.owner_token.empty() ? "" : ", owner token designates the player");
    // Without a join token anyone who reaches the port receives every
    // snapshot; an owner token alone only protects the character.
    if (!loopback && config.access.join_token.empty()) {
        std::fprintf(stderr, "[net] warning: anyone who can reach this port can join and %s; "
                             "set QUAT_NET_TOKEN\n",
                     config.access.owner_token.empty() ? "take the player character"
                                                       : "watch the session");
    }
    return session;
}

std::expected<NetSession, core::Error> NetSession::join(entt::registry& registry,
                                                        const JoinConfig& config) {
    if (config.token.size() > max_token_bytes) {
        return std::unexpected(core::make_error("net: join token too long"));
    }
    auto client = NetClient::connect(config.host, config.port);
    if (!client) return std::unexpected(client.error());

    NetSession session;
    session.client_ = std::move(*client);
    session.join_token_ = config.token;
    (void)assign_net_ids(registry);
    // The host simulates; local copies of its bodies only follow.
    const std::uint32_t replicas = make_replicas_kinematic(registry);
    std::fprintf(stderr, "[net] joining %s:%u (%u replicated bodies made kinematic)\n",
                 config.host.c_str(), static_cast<unsigned>(config.port), replicas);
    return session;
}

void NetSession::drive_character(entt::registry& registry, physics::PhysicsWorld* world,
                                 entt::entity character, const glm::vec3& move, float dt) {
    if (!controllable(registry, character)) return;
    auto& cc = registry.get<scene::CharacterController>(character);

    if (server_ && owner_ != invalid_client) return; // the remote player drives it
    if (!client_ || !client_->connected() || !hello_sent_) {
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
    // Nobody authenticated: nothing to send (a joiner's first snapshot is a
    // requested keyframe, so no delta baseline is needed meanwhile).
    if (server_ && !access_.recipients().empty()) {
        replication_.tick(registry, *server_, access_.recipients(), dt);
    }
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

// Ownership is decided by access_ (first authenticated / owner-token peer);
// this applies a change to the character.
void NetSession::sync_owner(entt::registry& registry, entt::entity character) {
    const ClientId owner = access_.owner();
    if (owner == owner_) return;
    if (owner_ != invalid_client) release_character(registry, character);
    owner_ = owner;
    inputs_.reset();
    if (owner_ != invalid_client) {
        std::fprintf(stderr, "[net] client %u controls the player character\n", owner_);
    }
}

void NetSession::drop_peer(ClientId client, CloseReason reason) {
    std::fprintf(stderr, "[net] dropping client %u: %s\n", client,
                 close_reason_name(static_cast<std::int32_t>(reason)).c_str());
    server_->disconnect(client, reason);
}

void NetSession::host_event(entt::registry& registry, entt::entity character,
                            const NetEvent& ev) {
    if (ev.type == NetEvent::Type::connected) {
        access_.on_connected(ev.client);
        std::fprintf(stderr, "[net] client %u connected, awaiting its hello\n", ev.client);
        return;
    }
    if (ev.type == NetEvent::Type::disconnected) {
        access_.on_disconnected(ev.client);
        std::fprintf(stderr, "[net] client %u left\n", ev.client);
        sync_owner(registry, character);
        return;
    }
    switch (const HostAccess::Decision d = access_.on_message(ev.client, ev.payload); d.verdict) {
    case HostAccess::Verdict::ignore:
        return;
    case HostAccess::Verdict::drop:
        drop_peer(ev.client, d.reason);
        sync_owner(registry, character);
        return;
    case HostAccess::Verdict::authenticated:
        std::fprintf(stderr, "[net] client %u authenticated\n", ev.client);
        replication_.request_keyframe(); // it has no baseline yet
        sync_owner(registry, character);
        return;
    case HostAccess::Verdict::deliver:
        break;
    }

    PlayerInput input;
    if (!decode_input(ev.payload, input)) {
        if (rejected_inputs_++ % 100 == 0) {
            std::fprintf(stderr, "[net] dropped malformed message from client %u\n", ev.client);
        }
        return;
    }
    if (ev.client != owner_) return; // authenticated, but not the player
    if (const InputQueue::Push r = inputs_.push(input); r != InputQueue::Push::queued) {
        if (rejected_inputs_++ % 100 == 0) {
            std::fprintf(stderr, "[net] input %u from client %u rejected: %s\n", input.sequence,
                         ev.client, push_result_name(r));
        }
    }
}

void NetSession::host_update(entt::registry& registry, physics::PhysicsWorld* world,
                             entt::entity character, float dt) {
    // Deadlines and rate budgets advance before this frame's messages.
    for (const ClientId late : access_.tick(dt)) drop_peer(late, CloseReason::auth_timeout);
    for (const NetEvent& ev : server_->poll()) host_event(registry, character, ev);

    if (owner_ == invalid_client) return;
    if (!controllable(registry, character)) {
        // No character to drive (deleted under the remote player): its
        // inputs are dropped until there is one again.
        inputs_.reset();
        return;
    }
    if (!registry.all_of<scene::NetDriven>(character)) {
        registry.emplace<scene::NetDriven>(character);
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

// The hello must be the first message the host sees; inputs
// (drive_character) only follow once it is queued, and reliable messages
// arrive in order.
void NetSession::try_send_hello() {
    if (hello_sent_ || !client_ || !client_->connected()) return;
    const auto hello = encode_hello(join_token_); // length checked in join()
    hello_sent_ = hello && client_->send(*hello, /*reliable=*/true);
    if (hello_sent_) {
        if (hello_refusals_ > 0) {
            std::fprintf(stderr, "[net] hello sent after %u refusals\n", hello_refusals_);
        }
        return;
    }
    if (hello_refusals_++ % 60 == 0) {
        std::fprintf(stderr, "[net] hello refused by the transport; retrying each frame "
                             "(inputs wait for it)\n");
    }
}

void NetSession::client_update(entt::registry& registry, physics::PhysicsWorld* world,
                               entt::entity character, float dt) {
    // Only the newest authoritative state of this poll matters.
    std::optional<std::pair<std::uint32_t, CharacterState>> latest;
    for (const NetEvent& ev : client_->poll()) {
        if (ev.type == NetEvent::Type::connected) {
            std::fprintf(stderr, "[net] connected to host\n");
            end_reason_ = 0;
            try_send_hello(); // first, before any input
            continue;
        }
        if (ev.type == NetEvent::Type::disconnected) {
            std::fprintf(stderr, "[net] disconnected from host: %s\n",
                         close_reason_name(ev.end_reason).c_str());
            end_reason_ = ev.end_reason;
            hello_sent_ = false;
            hello_refusals_ = 0;
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
    // A hello the transport refused is retried every frame until it goes out
    // (the host's auth deadline bounds how long that may take).
    if (!hello_sent_ && client_->connected()) try_send_hello();

    if (latest && controllable(registry, character)) {
        if (world == nullptr) {
            // No local physics to predict with: show the host's state.
            glm::mat4 pose = registry.get<scene::Transform>(character).world;
            pose[3] = glm::vec4(latest->second.position, 1.0F);
            scene::set_world_transform(registry, character, pose);
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
