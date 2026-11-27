// Networked character on real physics (S1 audit B1 + B5): host and client run
// the same Jolt CharacterVirtual step per input; the client's CharacterVirtual
// follows its prediction and is rewound / replayed on correction, instead of
// the controller and the Transform disagreeing.

#include <cstdint>
#include <optional>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/net/character_step.hpp"
#include "engine/net/prediction.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

namespace s = engine::scene;
namespace n = engine::net;
using engine::physics::PhysicsWorld;

constexpr float k_dt = 1.0F / 60.0F;

void add_static_box(PhysicsWorld& world, const glm::vec3& centre, const glm::vec3& half) {
    PhysicsWorld::BodyParams p;
    p.shape = world.create_box(half);
    p.position = centre;
    p.motion = engine::physics::Motion::static_body;
    p.layer = engine::physics::Layer::static_body;
    ASSERT_NE(world.add_body(p), PhysicsWorld::invalid_body);
}

// One end of the session: a physics world with a floor and the player.
struct End {
    std::optional<PhysicsWorld> world;
    entt::registry              registry;
    entt::entity                player = entt::null;

    End() {
        auto w = PhysicsWorld::create();
        EXPECT_TRUE(w.has_value());
        world = std::move(*w);
        add_static_box(*world, glm::vec3(0.0F, -1.0F, 0.0F), glm::vec3(50.0F, 1.0F, 50.0F));
        player = registry.create();
        auto& t = registry.emplace<s::Transform>(player);
        t.local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        t.world = t.local;
        registry.emplace<s::CharacterController>(player);
        registry.emplace<s::NetDriven>(player);
    }

    [[nodiscard]] n::CharacterPredictor::StepFn stepper() {
        return [this](const n::CharacterState& st, const n::PlayerInput& in) {
            return n::step_character_input(registry, *world, player, st, in);
        };
    }
    [[nodiscard]] n::CharacterState state() const { return n::character_state(registry, player); }
    [[nodiscard]] glm::vec3 virtual_position() const {
        return world->character_position(registry.get<s::CharacterController>(player).character);
    }
};

void expect_near(const glm::vec3& a, const glm::vec3& b, float tol) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

// Runs `frames` of client prediction + host simulation; the host's state is
// delivered every `ack_every` frames (a little latency).
struct Loop {
    End                    host;
    End                    client;
    n::InputQueue          queue;
    n::CharacterPredictor  predictor;
    std::uint32_t          sequence = 0;
    std::uint32_t          last_acked = 0;
    n::CharacterState      last_host_state;

    void frame(const glm::vec3& move, bool deliver) {
        const auto in = n::make_input(++sequence, move, k_dt);
        ASSERT_TRUE(in.has_value());
        (void)predictor.predict(client.state(), *in, client.stepper());
        // B5: the CharacterVirtual is where the prediction put the character.
        expect_near(client.virtual_position(), client.state().position, 1e-5F);

        ASSERT_EQ(queue.push(*in), n::InputQueue::Push::queued);
        queue.credit(k_dt);
        while (const auto taken = queue.take()) {
            last_host_state = host.stepper()(host.state(), *taken);
        }
        last_acked = queue.acked();
        if (deliver) reconcile();
    }

    void reconcile() {
        if (const auto corrected =
                predictor.reconcile(last_acked, last_host_state, client.stepper())) {
            n::restore_character(client.registry, *client.world, client.player, *corrected);
        }
        expect_near(client.virtual_position(), client.state().position, 1e-5F);
    }
};

TEST(NetCharacter, PredictionMatchesTheHostOnTheSameWorld) {
    Loop loop;
    // Land, then walk a curve.
    for (int f = 0; f < 240; ++f) {
        const float a = static_cast<float>(f) * 0.02F;
        loop.frame(f < 60 ? glm::vec3(0.0F) : glm::vec3(std::cos(a), 0.0F, std::sin(a)),
                   f % 4 == 3);
    }
    loop.reconcile();
    EXPECT_EQ(loop.predictor.corrections(), 0U) << "same world, same inputs: no corrections";
    EXPECT_TRUE(loop.client.state().on_ground);
    expect_near(loop.client.state().position, loop.host.state().position,
                n::CharacterPredictor::position_tolerance);
    EXPECT_GT(glm::length(glm::vec3(loop.host.state().position.x, 0.0F,
                                    loop.host.state().position.z)),
              1.0F)
        << "the host actually simulated the remote inputs";
}

TEST(NetCharacter, CorrectionMovesTheCharacterVirtual) {
    Loop loop;
    // A wall only the host has (the client's copy of the world is stale).
    add_static_box(*loop.host.world, glm::vec3(2.0F, 1.0F, 0.0F), glm::vec3(0.25F, 1.0F, 5.0F));
    for (int f = 0; f < 60; ++f) loop.frame(glm::vec3(0.0F), f % 4 == 3); // land
    for (int f = 0; f < 120; ++f) loop.frame(glm::vec3(1.0F, 0.0F, 0.0F), f % 4 == 3);
    loop.reconcile();

    EXPECT_GT(loop.predictor.corrections(), 0U);
    // After the final ack nothing is pending: the client stands where the host
    // does — in front of the wall — and its CharacterVirtual agrees.
    EXPECT_EQ(loop.predictor.pending(), 0U);
    const glm::vec3 host = loop.host.state().position;
    EXPECT_LT(host.x, 2.0F - 0.25F);
    expect_near(loop.client.state().position, host, 1e-4F);
    expect_near(loop.client.virtual_position(), host, 1e-4F);

    // And it keeps predicting from there (into the wall it doesn't know about
    // the client walks on; the host corrects it again).
    loop.frame(glm::vec3(1.0F, 0.0F, 0.0F), false);
    EXPECT_GT(loop.client.state().position.x, host.x);
}

TEST(NetCharacter, SetPositionRefreshesGroundState) {
    End end;
    s::step_character(end.registry, *end.world, end.player, k_dt); // creates the character
    const std::uint32_t id = end.registry.get<s::CharacterController>(end.player).character;
    end.world->character_set_position(id, glm::vec3(3.0F, 10.0F, 0.0F));
    expect_near(end.world->character_position(id), glm::vec3(3.0F, 10.0F, 0.0F), 1e-6F);
    EXPECT_FALSE(end.world->character_on_ground(id));

    // Standing on the floor: capsule centre at half_height + radius.
    const auto& cc = end.registry.get<s::CharacterController>(end.player);
    end.world->character_set_position(id, glm::vec3(3.0F, cc.half_height + cc.radius, 0.0F));
    EXPECT_TRUE(end.world->character_on_ground(id));
}

TEST(NetCharacter, NetDrivenCharactersAreSkippedByCharacterSystem) {
    End end;
    const glm::vec3 before = end.state().position;
    for (int i = 0; i < 30; ++i) s::character_system(end.registry, *end.world, k_dt);
    EXPECT_EQ(end.state().position, before) << "the network layer owns this character";
    end.registry.remove<s::NetDriven>(end.player);
    for (int i = 0; i < 30; ++i) s::character_system(end.registry, *end.world, k_dt);
    EXPECT_LT(end.state().position.y, before.y) << "local simulation resumes";
}

} // namespace
