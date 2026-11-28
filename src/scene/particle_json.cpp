// Codec of the particle_emitter component (R10.3) — see component_json.hpp.

#include <array>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "component_json_internal.hpp"
#include "engine/renderer/particles.hpp"
#include "engine/scene/component_json.hpp"

namespace engine::scene::detail {

namespace {

using json = nlohmann::json;
using core::EnumName;
using core::JsonFields;
namespace r = renderer;

constexpr EnumName<r::ParticleShape> k_shapes[] = {{"point", r::ParticleShape::point},
                                                   {"sphere", r::ParticleShape::sphere},
                                                   {"box", r::ParticleShape::box},
                                                   {"cone", r::ParticleShape::cone}};
constexpr EnumName<r::ParticleBlend> k_blends[] = {{"additive", r::ParticleBlend::additive},
                                                   {"alpha", r::ParticleBlend::alpha}};
constexpr EnumName<r::ParticleLighting> k_lighting[] = {{"unlit", r::ParticleLighting::unlit},
                                                        {"lit", r::ParticleLighting::lit}};
constexpr EnumName<r::ParticleSpace> k_spaces[] = {{"world", r::ParticleSpace::world},
                                                   {"local", r::ParticleSpace::local}};
constexpr std::size_t k_max_path = 4096;

template <typename E, std::size_t N>
[[nodiscard]] std::string name_of(const EnumName<E> (&names)[N], E value) {
    for (const auto& n : names) {
        if (n.value == value) return std::string(n.name);
    }
    return "unknown";
}

[[nodiscard]] json pair(const glm::vec2& v) { return json::array({v.x, v.y}); }
[[nodiscard]] json triple(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

// A vec3 whose components lie in [-limit, limit].
void read_bounded_vec3(JsonFields& f, std::string_view key, glm::vec3& out, float lo, float hi) {
    glm::vec3 v = out;
    f.vec3(key, v);
    if (glm::any(glm::lessThan(v, glm::vec3(lo))) || glm::any(glm::greaterThan(v, glm::vec3(hi)))) {
        f.fail(key, "components must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
        return;
    }
    out = v;
}

// A curve: 1..4 objects with ascending `t` in [0, 1]; `read` decodes the
// value fields of one key object into `keys[i]`.
template <typename Key, typename Read>
void read_curve(JsonFields& f, std::string_view key, std::array<Key, r::particle_curve_keys>& keys,
                std::uint32_t& count, Read&& read) {
    f.known({key});
    const json* v = f.raw(key);
    if (v == nullptr || v->is_null()) return;
    if (!v->is_array() || v->empty() || v->size() > r::particle_curve_keys) {
        f.fail(key, "expected 1 to 4 keys");
        return;
    }
    std::array<Key, r::particle_curve_keys> out = keys;
    float last_t = 0.0F;
    for (std::size_t i = 0; i < v->size(); ++i) {
        JsonFields k((*v)[i], std::string(key) + "[" + std::to_string(i) + "]");
        k.number("t", out[i].t, 0.0F, 1.0F);
        read(k, out[i]);
        if (auto ok = k.finish(); !ok) {
            f.fail(key, ok.error().message);
            return;
        }
        if (i > 0 && out[i].t < last_t) {
            f.fail(key, "keys must be in ascending t");
            return;
        }
        last_t = out[i].t;
    }
    keys = out;
    count = static_cast<std::uint32_t>(v->size());
}

} // namespace

json encode_particle_emitter(const r::ParticleEmitterSettings& s) {
    json size_curve = json::array();
    for (std::uint32_t i = 0; i < s.size_keys; ++i) {
        size_curve.push_back({{"t", s.size_curve[i].t}, {"size", s.size_curve[i].size}});
    }
    json color_curve = json::array();
    for (std::uint32_t i = 0; i < s.color_keys; ++i) {
        const glm::vec4& c = s.color_curve[i].color;
        color_curve.push_back({{"t", s.color_curve[i].t},
                               {"color", triple(glm::vec3(c))},
                               {"alpha", c.a}});
    }
    return {{"enabled", s.enabled},
            {"seed", s.seed},
            {"max_particles", s.max_particles},
            {"rate", s.rate},
            {"burst_count", s.burst_count},
            {"burst_interval", s.burst_interval},
            {"lifetime", pair(s.lifetime)},
            {"shape", name_of(k_shapes, s.shape)},
            {"radius", s.radius},
            {"box", triple(s.box_half_extents)},
            {"cone_angle", s.cone_angle_deg},
            {"direction", triple(s.direction)},
            {"spread", s.spread_deg},
            {"speed", pair(s.speed)},
            {"gravity", triple(s.gravity)},
            {"drag", s.drag},
            {"size", pair(s.size)},
            {"rotation", pair(s.rotation_deg)},
            {"spin", pair(s.spin_deg)},
            {"size_curve", std::move(size_curve)},
            {"color_curve", std::move(color_curve)},
            {"emissive", s.emissive},
            {"texture", s.texture},
            {"flipbook", json::array({s.flipbook_grid.x, s.flipbook_grid.y})},
            {"frames", s.flipbook_frames},
            {"fps", s.flipbook_fps},
            {"blend", name_of(k_blends, s.blend)},
            {"lighting", name_of(k_lighting, s.lighting)},
            {"space", name_of(k_spaces, s.space)},
            {"soft_distance", s.soft_distance},
            {"sort", s.sort},
            {"collide", s.collide},
            {"bounce", s.bounce},
            {"friction", s.friction}};
}

void decode_particle_emitter(JsonFields& f, r::ParticleEmitterSettings& s) {
    f.boolean("enabled", s.enabled).integer("seed", s.seed, 0, 0xFFFFFFFFLL);
    f.integer("max_particles", s.max_particles, 1, r::particle_max_per_emitter);
    f.number("rate", s.rate, 0.0F, 100000.0F);
    f.integer("burst_count", s.burst_count, 0, r::particle_max_per_emitter);
    f.number("burst_interval", s.burst_interval, 0.0F, 3600.0F);
    read_range(f, "lifetime", s.lifetime, 0.01F, 600.0F);

    f.enumeration("shape", s.shape, k_shapes).number("radius", s.radius, 0.0F, 1000.0F);
    read_bounded_vec3(f, "box", s.box_half_extents, 0.0F, 1000.0F);
    f.number("cone_angle", s.cone_angle_deg, 0.0F, 90.0F);
    glm::vec3 dir = s.direction;
    f.vec3("direction", dir);
    if (f.has("direction")) {
        if (glm::length(dir) < 1e-6F) {
            f.fail("direction", "must be non-zero");
        } else {
            s.direction = glm::normalize(dir);
        }
    }
    f.number("spread", s.spread_deg, 0.0F, 180.0F);
    read_range(f, "speed", s.speed, 0.0F, 1000.0F);
    read_bounded_vec3(f, "gravity", s.gravity, -1000.0F, 1000.0F);
    f.number("drag", s.drag, 0.0F, 100.0F);

    read_range(f, "size", s.size, 0.001F, 1000.0F);
    read_range(f, "rotation", s.rotation_deg, -3600.0F, 3600.0F);
    read_range(f, "spin", s.spin_deg, -3600.0F, 3600.0F);
    read_curve(f, "size_curve", s.size_curve, s.size_keys,
               [](JsonFields& k, r::ParticleSizeKey& key) { k.number("size", key.size, 0.0F, 1000.0F); });
    read_curve(f, "color_curve", s.color_curve, s.color_keys,
               [](JsonFields& k, r::ParticleColorKey& key) {
                   k.known({"color"});
                   if (const json* c = k.raw("color")) {
                       if (auto ok = decode_color(*c, key.color); !ok) k.fail("color", ok.error().message);
                   }
                   k.number("alpha", key.color.a, 0.0F, 1.0F);
               });
    f.number("emissive", s.emissive, 0.0F, 1000.0F);

    f.string("texture", s.texture, k_max_path);
    check_project_path(f, "texture", s.texture);
    if (!s.texture.empty() && s.texture.rfind("assets/", 0) != 0) {
        f.fail("texture", "expected a path under assets/");
    }
    f.known({"flipbook"});
    if (const json* g = f.raw("flipbook"); g != nullptr && !g->is_null()) {
        std::int64_t cols = 0;
        std::int64_t rows = 0;
        if (g->is_array() && g->size() == 2 && JsonFields::read_int((*g)[0], cols) &&
            JsonFields::read_int((*g)[1], rows) && cols >= 1 && cols <= 64 && rows >= 1 &&
            rows <= 64) {
            s.flipbook_grid = glm::uvec2(static_cast<std::uint32_t>(cols),
                                         static_cast<std::uint32_t>(rows));
        } else {
            f.fail("flipbook", "expected [columns, rows], each in [1, 64]");
        }
    }
    f.integer("frames", s.flipbook_frames, 1, 4096).number("fps", s.flipbook_fps, 0.0F, 240.0F);
    if (s.flipbook_frames > s.flipbook_grid.x * s.flipbook_grid.y) {
        f.fail("frames", "must be at most columns x rows of the flipbook");
    }
    f.enumeration("blend", s.blend, k_blends).enumeration("lighting", s.lighting, k_lighting);
    f.enumeration("space", s.space, k_spaces);
    f.number("soft_distance", s.soft_distance, 0.0F, 100.0F).boolean("sort", s.sort);
    f.boolean("collide", s.collide).number("bounce", s.bounce, 0.0F, 1.0F);
    f.number("friction", s.friction, 0.0F, 1.0F);
}

} // namespace engine::scene::detail
