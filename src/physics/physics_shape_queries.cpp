// PhysicsWorld multi-hit queries (A10.1): every hit along a ray, shape
// overlaps and shape casts, filtered by object layer. See
// include/engine/physics/physics_world.hpp; shared internals in
// physics_world_impl.hpp.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "physics_world_impl.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsuggest-override"
#endif
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace engine::physics {

using namespace detail;

namespace {

class LayerMaskFilter final : public JPH::ObjectLayerFilter {
public:
    explicit LayerMaskFilter(LayerMask mask) : mask_(mask) {}
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return layer < 32 && (mask_ & (LayerMask{1} << layer)) != 0;
    }

private:
    LayerMask mask_;
};

[[nodiscard]] JPH::ShapeRefC query_shape(const PhysicsWorld::ShapeDesc& d) {
    switch (d.type) {
    case PhysicsWorld::QueryShape::sphere:
        return new JPH::SphereShape(d.radius);
    case PhysicsWorld::QueryShape::box:
        return new JPH::BoxShape(to_jph(d.half_extents),
                                 std::min({d.half_extents.x, d.half_extents.y, d.half_extents.z,
                                           JPH::cDefaultConvexRadius}));
    case PhysicsWorld::QueryShape::capsule:
        return new JPH::CapsuleShape(d.half_height, d.radius);
    }
    return new JPH::SphereShape(d.radius);
}

[[nodiscard]] bool valid_shape(const PhysicsWorld::ShapeDesc& d) {
    switch (d.type) {
    case PhysicsWorld::QueryShape::sphere:
        return d.radius > 0.0F && std::isfinite(d.radius);
    case PhysicsWorld::QueryShape::box:
        return glm::all(glm::greaterThan(d.half_extents, glm::vec3(0.0F))) &&
               glm::all(glm::lessThan(glm::abs(d.half_extents), glm::vec3(1e30F)));
    case PhysicsWorld::QueryShape::capsule:
        return d.radius > 0.0F && d.half_height >= 0.0F && std::isfinite(d.radius) &&
               std::isfinite(d.half_height);
    }
    return false;
}

} // namespace

void PhysicsWorld::Impl::describe_hit(const PhysicsWorld& world, JPH::BodyID id,
                                      QueryHit& hit) const {
    hit.body = id.GetIndexAndSequenceNumber();
    hit.owner = world.body_owner(hit.body);
    if (const auto it = inner_bodies.find(hit.body); it != inner_bodies.end()) {
        hit.character = it->second;
    }
    const JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
    if (lock.Succeeded()) {
        hit.layer = static_cast<Layer>(lock.GetBody().GetObjectLayer());
        hit.sensor = lock.GetBody().IsSensor();
    }
}

std::vector<PhysicsWorld::QueryHit> PhysicsWorld::cast_ray_all(const glm::vec3& origin,
                                                               const glm::vec3& direction,
                                                               float max_distance,
                                                               LayerMask layers,
                                                               std::size_t max_hits) const {
    std::vector<QueryHit> out;
    const float len = glm::length(direction);
    if (!impl_ || !(len > 1e-6F) || !(max_distance > 0.0F) || !std::isfinite(max_distance)) {
        return out;
    }
    max_hits = std::min(max_hits, max_query_hits);
    const glm::vec3 dir = direction / len;
    const JPH::RRayCast ray(to_jph_r(origin), to_jph(dir * max_distance));
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const LayerMaskFilter filter(layers);
    impl_->system.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings{}, collector,
                                                JPH::BroadPhaseLayerFilter{}, filter);
    collector.Sort();
    std::unordered_set<std::uint32_t> seen; // one hit per body: the nearest
    for (const JPH::RayCastResult& r : collector.mHits) {
        if (out.size() >= max_hits) break;
        if (!seen.insert(r.mBodyID.GetIndexAndSequenceNumber()).second) continue;
        QueryHit hit;
        impl_->describe_hit(*this, r.mBodyID, hit);
        const JPH::RVec3 point = ray.GetPointOnRay(r.mFraction);
        hit.point = to_glm(point);
        hit.fraction = r.mFraction;
        hit.distance = r.mFraction * max_distance;
        const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), r.mBodyID);
        if (lock.Succeeded()) {
            hit.normal = to_glm(lock.GetBody().GetWorldSpaceSurfaceNormal(r.mSubShapeID2, point));
        }
        out.push_back(hit);
    }
    return out;
}

std::vector<PhysicsWorld::QueryHit> PhysicsWorld::overlap(const ShapeDesc& shape,
                                                          const glm::vec3& position,
                                                          const glm::quat& rotation,
                                                          LayerMask layers,
                                                          std::size_t max_hits) const {
    if (!impl_ || !valid_shape(shape)) return {};
    max_hits = std::min(max_hits, max_query_hits);
    const JPH::ShapeRefC s = query_shape(shape);
    const JPH::RVec3 base = to_jph_r(position);
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(to_jph(rotation), base);
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const LayerMaskFilter filter(layers);
    impl_->system.GetNarrowPhaseQuery().CollideShape(
        s, JPH::Vec3::sOne(), transform.PreTranslated(s->GetCenterOfMass()), settings, base,
        collector, JPH::BroadPhaseLayerFilter{}, filter);

    // Deepest contact per body, deepest bodies first.
    std::unordered_map<std::uint32_t, std::size_t> index;
    std::vector<QueryHit> hits;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const std::uint32_t id = r.mBodyID2.GetIndexAndSequenceNumber();
        auto [it, fresh] = index.emplace(id, hits.size());
        if (!fresh && hits[it->second].penetration >= r.mPenetrationDepth) continue;
        QueryHit hit;
        impl_->describe_hit(*this, r.mBodyID2, hit);
        hit.point = to_glm(base + r.mContactPointOn2);
        const float axis_len = r.mPenetrationAxis.Length();
        if (axis_len > 0.0F) hit.normal = -to_glm(r.mPenetrationAxis / axis_len);
        hit.penetration = r.mPenetrationDepth;
        if (fresh) {
            hits.push_back(hit);
        } else {
            hits[it->second] = hit;
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const QueryHit& a, const QueryHit& b) {
        return a.penetration > b.penetration;
    });
    if (hits.size() > max_hits) hits.resize(max_hits);
    return hits;
}

std::vector<PhysicsWorld::QueryHit> PhysicsWorld::cast_shape(const ShapeDesc& shape,
                                                             const glm::vec3& from,
                                                             const glm::vec3& to,
                                                             const glm::quat& rotation,
                                                             LayerMask layers,
                                                             std::size_t max_hits) const {
    std::vector<QueryHit> out;
    const glm::vec3 delta = to - from;
    if (!impl_ || !valid_shape(shape) || !(glm::length(delta) > 1e-6F)) return out;
    max_hits = std::min(max_hits, max_query_hits);
    const JPH::ShapeRefC s = query_shape(shape);
    const JPH::RVec3 base = to_jph_r(from);
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        s, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(to_jph(rotation), base),
        to_jph(delta));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mReturnDeepestPoint = true;
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    const LayerMaskFilter filter(layers);
    impl_->system.GetNarrowPhaseQuery().CastShape(cast, settings, base, collector,
                                                  JPH::BroadPhaseLayerFilter{}, filter);
    collector.Sort();
    std::unordered_set<std::uint32_t> seen; // nearest hit per body
    const float length = glm::length(delta);
    for (const JPH::ShapeCastResult& r : collector.mHits) {
        if (out.size() >= max_hits) break;
        if (!seen.insert(r.mBodyID2.GetIndexAndSequenceNumber()).second) continue;
        QueryHit hit;
        impl_->describe_hit(*this, r.mBodyID2, hit);
        hit.point = to_glm(base + r.mContactPointOn2);
        const float axis_len = r.mPenetrationAxis.Length();
        if (axis_len > 0.0F) hit.normal = -to_glm(r.mPenetrationAxis / axis_len);
        hit.fraction = r.mFraction;
        hit.distance = r.mFraction * length;
        hit.penetration = r.mFraction <= 0.0F ? r.mPenetrationDepth : 0.0F;
        out.push_back(hit);
    }
    return out;
}

} // namespace engine::physics
