#include "asset-gen/nature_models.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace quat::asset_gen {
namespace {

constexpr float k_tau = 6.28318530718F;
// Trunks start this far below the origin so a slope never shows their foot.
constexpr float k_trunk_sink = 0.5F;

Material make_material(std::string name, const glm::vec3& color, float roughness) {
    return Material{std::move(name), glm::vec4(color, 1.0F), 0.0F, roughness};
}

// Pushes each ring point radially (from the ring centre) by up to `amount`
// of its distance and vertically by up to `lift` metres.
void jitter_ring(Ring& ring, Rng& rng, float amount, float lift) {
    for (glm::vec3& p : ring.points) {
        const glm::vec3 d = p - ring.centre;
        p = ring.centre + d * (1.0F + rng.uniform(-amount, amount));
        p.y += rng.uniform(-lift, lift);
    }
}

} // namespace

Model build_conifer(std::string_view name, const ConiferParams& params, std::uint32_t seed) {
    Rng rng(seed);
    Model m;
    m.name = std::string(name);
    m.materials = {make_material("bark", params.bark, 0.92F),
                   make_material("needles_a", params.needles_a, 0.8F),
                   make_material("needles_b", params.needles_b, 0.8F)};

    const float h = params.height;
    const float trunk_r = params.radius * 0.085F;
    add_frustum(m.part(0), glm::vec3(0.0F, -k_trunk_sink, 0.0F), glm::vec3(0.0F, h * 0.9F, 0.0F),
                trunk_r, trunk_r * 0.25F, std::max(params.trunk_sides, 3), rng.uniform(0.0F, k_tau));

    const int tiers = std::max(params.tiers, 1);
    const float crown_base = params.crown_start * h;
    const float spacing = (h - crown_base) / (static_cast<float>(tiers) + params.overlap);
    const int sides = std::max(params.sides, 3);
    for (int i = 0; i < tiers; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(tiers);
        const float base_y = crown_base + static_cast<float>(i) * spacing;
        const bool top = i == tiers - 1;
        const float tier_h = top ? h - base_y : spacing * (1.0F + params.overlap);
        const float r = std::max(params.radius * std::pow(1.0F - t, params.taper) *
                                     rng.uniform(0.92F, 1.08F),
                                 0.25F);
        const float sway_x = rng.uniform(-params.sway, params.sway);
        const float sway_z = rng.uniform(-params.sway, params.sway);
        const glm::vec3 axis_off(sway_x, 0.0F, sway_z);
        const float phase = rng.uniform(0.0F, k_tau);
        const glm::vec3 up(0.0F, 1.0F, 0.0F);

        Ring rim = make_ring(axis_off + glm::vec3(0.0F, base_y - params.droop * r, 0.0F), up, r,
                             sides, phase);
        jitter_ring(rim, rng, params.jitter, 0.08F * r);
        Ring mid = make_ring(axis_off * 0.6F + glm::vec3(0.0F, base_y + 0.42F * tier_h, 0.0F), up,
                             r * 0.52F, sides, phase + 0.5F * k_tau / static_cast<float>(sides));
        jitter_ring(mid, rng, params.jitter * 0.8F, 0.04F * r);
        const float apex_x = rng.uniform(-0.05F, 0.05F) * r;
        const float apex_z = rng.uniform(-0.05F, 0.05F) * r;
        const glm::vec3 apex = axis_off * 0.3F + glm::vec3(apex_x, base_y + tier_h, apex_z);
        const glm::vec3 underside = axis_off + glm::vec3(0.0F, base_y + 0.18F * tier_h, 0.0F);

        // Lower tiers lean to the darker needle colour (self-shadowed mass).
        const std::uint32_t mat = rng.uniform(0.0F, 1.0F) < 0.35F + 0.5F * t ? 2U : 1U;
        Primitive& needles = m.part(mat);
        if (params.mid_ring) {
            add_band(needles, rim, mid);
            add_fan(needles, mid, apex, true);
        } else {
            add_fan(needles, rim, apex, true);
        }
        if (params.underside) add_fan(needles, rim, underside, false);
    }
    return m;
}

Model build_broadleaf(std::string_view name, const BroadleafParams& params, std::uint32_t seed) {
    Rng rng(seed);
    Model m;
    m.name = std::string(name);
    m.materials = {make_material("bark", params.bark, 0.9F),
                   make_material("leaves_a", params.leaves_a, 0.75F),
                   make_material("leaves_b", params.leaves_b, 0.75F)};

    const float h = params.height;
    const float cr = params.crown_radius;
    const float tr = params.trunk_radius;
    const glm::vec3 fork(0.0F, h * 0.45F, 0.0F);
    add_frustum(m.part(0), glm::vec3(0.0F, -k_trunk_sink, 0.0F), fork + glm::vec3(0.0F, 0.3F, 0.0F),
                tr, tr * 0.7F, 7, rng.uniform(0.0F, k_tau));

    std::uint32_t blob_seed = seed * 7919U + 13U;
    const auto add_canopy_blob = [&](const glm::vec3& centre, float radius) {
        BlobParams b;
        b.centre = centre;
        const float scale_x = rng.uniform(0.9F, 1.1F);
        const float scale_z = rng.uniform(0.9F, 1.1F);
        b.radii = glm::vec3(radius, radius * params.squash, radius) *
                  glm::vec3(scale_x, 1.0F, scale_z);
        b.subdivisions = 1;
        b.roughness = 0.28F;
        b.frequency = 2.2F;
        b.seed = blob_seed++;
        add_blob(m.part(rng.uniform(0.0F, 1.0F) < 0.5F ? 1U : 2U), b);
    };

    const int branches = std::max(params.branches, 1);
    const float phase = rng.uniform(0.0F, k_tau);
    for (int b = 0; b < branches; ++b) {
        const float az = phase + k_tau * static_cast<float>(b) / static_cast<float>(branches) +
                         rng.uniform(-0.3F, 0.3F);
        const float el = rng.uniform(0.6F, 0.95F); // radians above the horizontal
        const glm::vec3 dir(std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az));
        const glm::vec3 start = fork + glm::vec3(0.0F, rng.uniform(-0.4F, 0.3F), 0.0F);
        const glm::vec3 end = start + dir * (cr * rng.uniform(0.75F, 1.0F));
        add_frustum(m.part(0), start, end, tr * 0.5F, tr * 0.22F, 5, rng.uniform(0.0F, k_tau));
        for (int k = 0; k < std::max(params.blobs_per_branch, 1); ++k) {
            const float off_x = rng.uniform(-0.35F, 0.35F) * cr;
            const float off_y = rng.uniform(0.0F, 0.35F) * cr;
            const float off_z = rng.uniform(-0.35F, 0.35F) * cr;
            const glm::vec3 off(off_x, off_y, off_z);
            add_canopy_blob(end + off, cr * rng.uniform(0.42F, 0.6F));
        }
    }
    // A crowning blob over the fork closes the canopy.
    add_canopy_blob(glm::vec3(0.0F, h - cr * 0.75F * params.squash, 0.0F), cr * 0.68F);
    return m;
}

Model build_rock(std::string_view name, const RockParams& params, std::uint32_t seed) {
    Rng rng(seed);
    Model m;
    m.name = std::string(name);
    m.materials = {make_material("rock", params.color, 0.86F)};
    BlobParams b;
    const float scale_x = rng.uniform(0.9F, 1.1F);
    const float scale_y = rng.uniform(0.9F, 1.1F);
    const float scale_z = rng.uniform(0.9F, 1.1F);
    b.radii = params.radii * glm::vec3(scale_x, scale_y, scale_z);
    b.centre = glm::vec3(0.0F, b.radii.y * (1.0F - 2.0F * params.embed), 0.0F);
    b.subdivisions = params.subdivisions;
    b.roughness = params.roughness;
    b.frequency = params.frequency;
    b.seed = seed * 2654435761U + 1U;
    add_blob(m.part(0), b);
    return m;
}

Model build_bush(std::string_view name, const BushParams& params, std::uint32_t seed) {
    Rng rng(seed);
    Model m;
    m.name = std::string(name);
    m.materials = {make_material("leaves_a", params.leaves_a, 0.8F),
                   make_material("leaves_b", params.leaves_b, 0.8F)};
    const int blobs = std::max(params.blobs, 1);
    for (int i = 0; i < blobs; ++i) {
        const float a = k_tau * static_cast<float>(i) / static_cast<float>(blobs) +
                        rng.uniform(-0.4F, 0.4F);
        const float d = i == 0 ? 0.0F : params.spread * rng.uniform(0.5F, 1.0F);
        BlobParams b;
        const float r = params.radius * rng.uniform(0.75F, 1.15F) * (i == 0 ? 1.2F : 1.0F);
        b.centre = glm::vec3(std::cos(a) * d, r * rng.uniform(0.55F, 0.8F) + (i == 0 ? 0.25F : 0.0F),
                             std::sin(a) * d);
        b.radii = glm::vec3(r, r * 0.85F, r);
        b.subdivisions = 1;
        b.roughness = 0.3F;
        b.frequency = 2.5F;
        b.seed = seed * 40503U + static_cast<std::uint32_t>(i);
        b.floor_y = -0.15F;
        add_blob(m.part(i % 2 == 0 ? 0U : 1U), b);
    }
    return m;
}

namespace {

// The distant-forest version of a conifer: the same silhouette from plain
// six-sided cones on a four-sided trunk (no mid rings or undersides; about a
// fifth of the triangles). Seen from hundreds of metres the difference does
// not show.
ConiferParams far_detail(ConiferParams p) {
    p.sides = 6;
    p.trunk_sides = 4;
    p.mid_ring = false;
    p.underside = false;
    return p;
}

ConiferParams spruce_params() {
    ConiferParams p;
    p.height = 13.0F;
    p.radius = 2.7F;
    p.tiers = 7;
    p.crown_start = 0.1F;
    return p;
}

ConiferParams fir_params() {
    ConiferParams p;
    p.height = 10.0F;
    p.radius = 3.1F;
    p.tiers = 5;
    p.crown_start = 0.08F;
    p.overlap = 1.1F;
    p.droop = 0.24F;
    p.taper = 0.75F;
    p.needles_a = {0.022F, 0.05F, 0.035F};
    p.needles_b = {0.03F, 0.062F, 0.04F};
    return p;
}

ConiferParams pine_params() {
    ConiferParams p;
    p.height = 14.0F;
    p.radius = 2.2F;
    p.tiers = 4;
    p.crown_start = 0.42F;
    p.overlap = 0.7F;
    p.droop = 0.12F;
    p.jitter = 0.22F;
    p.sway = 0.45F;
    p.taper = 0.6F;
    p.bark = {0.16F, 0.08F, 0.045F};
    p.needles_a = {0.035F, 0.065F, 0.025F};
    p.needles_b = {0.045F, 0.075F, 0.028F};
    return p;
}

Model spruce(std::uint32_t seed) { return build_conifer("conifer_spruce", spruce_params(), seed); }
Model fir(std::uint32_t seed) { return build_conifer("conifer_fir", fir_params(), seed); }
Model pine(std::uint32_t seed) { return build_conifer("conifer_pine", pine_params(), seed); }
Model spruce_far(std::uint32_t seed) {
    return build_conifer("conifer_spruce_far", far_detail(spruce_params()), seed);
}
Model fir_far(std::uint32_t seed) {
    return build_conifer("conifer_fir_far", far_detail(fir_params()), seed);
}
Model pine_far(std::uint32_t seed) {
    return build_conifer("conifer_pine_far", far_detail(pine_params()), seed);
}

Model oak(std::uint32_t seed) {
    BroadleafParams p;
    return build_broadleaf("broadleaf_oak", p, seed);
}

Model birch(std::uint32_t seed) {
    BroadleafParams p;
    p.height = 12.0F;
    p.crown_radius = 2.6F;
    p.branches = 3;
    p.blobs_per_branch = 2;
    p.trunk_radius = 0.2F;
    p.squash = 0.95F;
    p.bark = {0.62F, 0.60F, 0.55F};
    p.leaves_a = {0.2F, 0.15F, 0.025F}; // turning: golden autumn foliage
    p.leaves_b = {0.13F, 0.14F, 0.03F};
    return build_broadleaf("broadleaf_birch", p, seed);
}

Model boulder(std::uint32_t seed) {
    RockParams p;
    return build_rock("rock_boulder", p, seed);
}

Model slab(std::uint32_t seed) {
    RockParams p;
    p.radii = {1.7F, 0.55F, 1.15F};
    p.subdivisions = 1;
    p.roughness = 0.3F;
    p.frequency = 1.4F;
    p.embed = 0.25F;
    p.color = {0.18F, 0.175F, 0.16F};
    return build_rock("rock_slab", p, seed);
}

Model spire(std::uint32_t seed) {
    RockParams p;
    p.radii = {0.75F, 1.6F, 0.7F};
    p.subdivisions = 1;
    p.roughness = 0.26F;
    p.frequency = 2.2F;
    p.color = {0.2F, 0.21F, 0.17F}; // lichen-tinted
    return build_rock("rock_spire", p, seed);
}

Model bush(std::uint32_t seed) {
    BushParams p;
    return build_bush("bush", p, seed);
}

constexpr std::array<NatureAsset, 12> k_catalogue = {{
    {"conifer_spruce.glb", NatureKind::conifer, &spruce},
    {"conifer_fir.glb", NatureKind::conifer, &fir},
    {"conifer_pine.glb", NatureKind::conifer, &pine},
    {"conifer_spruce_far.glb", NatureKind::conifer, &spruce_far},
    {"conifer_fir_far.glb", NatureKind::conifer, &fir_far},
    {"conifer_pine_far.glb", NatureKind::conifer, &pine_far},
    {"broadleaf_oak.glb", NatureKind::broadleaf, &oak},
    {"broadleaf_birch.glb", NatureKind::broadleaf, &birch},
    {"rock_boulder.glb", NatureKind::rock, &boulder},
    {"rock_slab.glb", NatureKind::rock, &slab},
    {"rock_spire.glb", NatureKind::rock, &spire},
    {"bush.glb", NatureKind::bush, &bush},
}};

} // namespace

std::span<const NatureAsset> nature_catalogue() { return k_catalogue; }

std::uint32_t asset_seed(std::uint32_t seed, std::size_t index) {
    std::uint32_t h = seed ^ (static_cast<std::uint32_t>(index) * 0x9E3779B9U);
    h ^= h >> 16U;
    h *= 0x7FEB352DU;
    h ^= h >> 15U;
    h *= 0x846CA68BU;
    h ^= h >> 16U;
    return h;
}

} // namespace quat::asset_gen
