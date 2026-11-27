// PhysicsWorld character controllers (JPH::CharacterVirtual). See
// include/engine/physics/physics_world.hpp; shared internals in
// physics_world_impl.hpp.

#include "physics_world_impl.hpp"

namespace engine::physics {

using namespace detail;

std::uint32_t PhysicsWorld::create_character(float half_height, float radius,
                                             const glm::vec3& position, std::uint32_t owner) {
    if (!(radius > 0.0F) || !(half_height >= 0.0F)) return invalid_body;
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = new JPH::CapsuleShape(half_height, radius);
    // Accept ground contacts within the bottom hemisphere as "supporting".
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        settings, to_jph_r(position), JPH::Quat::sIdentity(), 0, &impl_->system);
    return impl_->characters.insert(CharacterRecord{std::move(character), owner});
}

void PhysicsWorld::remove_character(std::uint32_t character) {
    if (!impl_) return;
    impl_->characters.erase(character); // releases the CharacterVirtual, frees the slot
}

std::vector<PhysicsWorld::OwnedBody> PhysicsWorld::owned_characters() const {
    std::vector<OwnedBody> out;
    impl_->characters.for_each([&](std::uint32_t id, const CharacterRecord& c) {
        if (c.owner != no_owner) out.push_back({id, c.owner});
    });
    return out;
}

void PhysicsWorld::character_set_velocity(std::uint32_t character, const glm::vec3& velocity) {
    if (JPH::CharacterVirtual* c = impl_->character(character)) {
        c->SetLinearVelocity(to_jph(velocity));
    }
}

glm::vec3 PhysicsWorld::character_velocity(std::uint32_t character) const {
    const JPH::CharacterVirtual* c = impl_->character(character);
    return c != nullptr ? to_glm(c->GetLinearVelocity()) : glm::vec3(0.0F);
}

void PhysicsWorld::character_update(std::uint32_t character, float dt) {
    JPH::CharacterVirtual* c = impl_->character(character);
    if (c == nullptr) return;
    const JPH::ObjectLayer layer = object_layer_of(Layer::character);
    const JPH::DefaultBroadPhaseLayerFilter bp_filter(impl_->obj_vs_bp, layer);
    const JPH::DefaultObjectLayerFilter obj_filter(impl_->obj_pair, layer);
    JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
    c->ExtendedUpdate(dt, impl_->system.GetGravity(), update_settings, bp_filter, obj_filter,
                      JPH::BodyFilter{}, JPH::ShapeFilter{}, *impl_->temp_allocator);
}

glm::vec3 PhysicsWorld::character_position(std::uint32_t character) const {
    const JPH::CharacterVirtual* c = impl_->character(character);
    return c != nullptr ? to_glm(c->GetPosition()) : glm::vec3(0.0F);
}

void PhysicsWorld::character_set_position(std::uint32_t character, const glm::vec3& position) {
    JPH::CharacterVirtual* c = impl_->character(character);
    if (c == nullptr) return;
    c->SetPosition(to_jph_r(position));
    // SetPosition keeps the contacts of the old location; re-detect them so
    // the ground state and the next update start from the new one.
    const JPH::ObjectLayer layer = object_layer_of(Layer::character);
    const JPH::DefaultBroadPhaseLayerFilter bp_filter(impl_->obj_vs_bp, layer);
    const JPH::DefaultObjectLayerFilter obj_filter(impl_->obj_pair, layer);
    c->RefreshContacts(bp_filter, obj_filter, JPH::BodyFilter{}, JPH::ShapeFilter{},
                       *impl_->temp_allocator);
}

bool PhysicsWorld::character_on_ground(std::uint32_t character) const {
    const JPH::CharacterVirtual* c = impl_->character(character);
    return c != nullptr && c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

} // namespace engine::physics
