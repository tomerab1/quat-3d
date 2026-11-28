#include "engine/renderer/draw_batching.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <unordered_map>

namespace engine::renderer {

namespace {

// Batch key: draws merge when they share mesh and material (by identity).
struct BatchKey {
    const asset::MeshAsset*     mesh;
    const asset::MaterialAsset* material;

    bool operator==(const BatchKey&) const = default;
};

struct BatchKeyHash {
    std::size_t operator()(const BatchKey& k) const noexcept {
        const std::size_t a = std::hash<const void*>{}(k.mesh);
        const std::size_t b = std::hash<const void*>{}(k.material);
        return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    }
};

constexpr std::uint32_t kDropped = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] InstanceMotion motion_for(const DrawItem& item) {
    return make_instance_motion(item.prev_model.value_or(item.model), item.prev_positions);
}

[[nodiscard]] InstancedDraw draw_for(const DrawItem& item) {
    InstancedDraw d;
    d.mesh = item.mesh;
    d.material = item.material;
    d.skinned_vertices = item.skinned_vertices;
    return d;
}

} // namespace

InstanceData make_instance_data(const glm::mat4& model) {
    // Cofactor matrix of the upper 3x3 by columns: (b x c, c x a, a x b) for
    // columns a, b, c. Equal to det(M) * M^-T but needs no division, so it is
    // exact for singular transforms too.
    const glm::vec3 a(model[0]);
    const glm::vec3 b(model[1]);
    const glm::vec3 c(model[2]);
    const glm::vec3 c0 = glm::cross(b, c);
    const glm::vec3 c1 = glm::cross(c, a);
    const glm::vec3 c2 = glm::cross(a, b);
    // det(M) = a . (b x c). A singular transform counts as non-mirrored.
    const float sign = glm::dot(a, c0) < 0.0F ? -1.0F : 1.0F;

    InstanceData out;
    out.model = model;
    out.normal = {glm::vec4(c0 * sign, sign), glm::vec4(c1 * sign, 0.0F),
                  glm::vec4(c2 * sign, 0.0F)};
    return out;
}

InstancedDrawList batch_draws(std::span<const DrawItem> items) {
    InstancedDrawList out;
    std::vector<std::uint32_t> draw_of(items.size(), kDropped);
    std::unordered_map<BatchKey, std::uint32_t, BatchKeyHash> batch_of;
    batch_of.reserve(items.size());

    // Pass 1: assign every item to a draw (first-appearance order) and count.
    for (std::size_t i = 0; i < items.size(); ++i) {
        const DrawItem& item = items[i];
        if (item.mesh == nullptr) continue;
        const auto next = static_cast<std::uint32_t>(out.draws.size());
        std::uint32_t d = next;
        if (item.skinned_vertices == VK_NULL_HANDLE) {
            d = batch_of.try_emplace(BatchKey{item.mesh, item.material}, next).first->second;
        }
        if (d == next) out.draws.push_back(draw_for(item));
        ++out.draws[d].instance_count;
        draw_of[i] = d;
    }

    // Pass 2: contiguous instance ranges in draw order.
    std::uint32_t total = 0;
    for (InstancedDraw& d : out.draws) {
        d.first_instance = total;
        total += d.instance_count;
    }

    // Pass 3: scatter the instance records (items keep their relative order).
    out.instances.resize(total);
    out.motion.resize(total);
    std::vector<std::uint32_t> cursor(out.draws.size());
    for (std::size_t d = 0; d < out.draws.size(); ++d) cursor[d] = out.draws[d].first_instance;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (draw_of[i] == kDropped) continue;
        const std::uint32_t slot = cursor[draw_of[i]]++;
        out.instances[slot] = make_instance_data(items[i].model);
        out.motion[slot] = motion_for(items[i]);
    }
    return out;
}

InstancedDrawList unbatched_draws(std::span<const DrawItem> items) {
    InstancedDrawList out;
    out.instances.reserve(items.size());
    out.motion.reserve(items.size());
    out.draws.reserve(items.size());
    for (const DrawItem& item : items) {
        if (item.mesh == nullptr) continue;
        InstancedDraw d = draw_for(item);
        d.first_instance = static_cast<std::uint32_t>(out.instances.size());
        d.instance_count = 1;
        out.draws.push_back(d);
        out.instances.push_back(make_instance_data(item.model));
        out.motion.push_back(motion_for(item));
    }
    return out;
}

std::vector<std::uint32_t> partition_mirrored(InstancedDrawList& list) {
    std::vector<std::uint32_t> unmirrored;
    unmirrored.reserve(list.draws.size());
    const bool with_motion = list.motion.size() == list.instances.size();
    std::vector<InstanceData>   instances;
    std::vector<InstanceMotion> motion;
    for (const InstancedDraw& d : list.draws) {
        const std::size_t first = d.first_instance;
        const std::size_t end = std::min(first + d.instance_count, list.instances.size());
        instances.clear();
        motion.clear();
        for (int pass = 0; pass < 2; ++pass) { // unmirrored, then mirrored
            for (std::size_t i = first; i < end; ++i) {
                if (is_mirrored(list.instances[i]) != (pass == 1)) continue;
                instances.push_back(list.instances[i]);
                if (with_motion) motion.push_back(list.motion[i]);
            }
            if (pass == 0) unmirrored.push_back(static_cast<std::uint32_t>(instances.size()));
        }
        std::copy(instances.begin(), instances.end(),
                  list.instances.begin() + static_cast<std::ptrdiff_t>(first));
        if (with_motion) {
            std::copy(motion.begin(), motion.end(),
                      list.motion.begin() + static_cast<std::ptrdiff_t>(first));
        }
    }
    return unmirrored;
}

} // namespace engine::renderer
