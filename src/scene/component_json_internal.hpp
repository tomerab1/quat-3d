#pragma once

// Private to the component codec (component_json.cpp and the per-component
// codec files next to it): shared field readers and the codecs that live in
// their own files.

#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"

namespace engine::renderer {
struct ParticleEmitterSettings;
}

namespace engine::scene::detail {

// Fails `key` unless `path` is empty or relative to the project root without
// climbing out of it (remote clients write these paths).
void check_project_path(core::JsonFields& f, std::string_view key, const std::string& path);

// Reads a [min, max] pair within [lo, hi] (min <= max).
void read_range(core::JsonFields& f, std::string_view key, glm::vec2& out, float lo, float hi);

// particle_emitter (particle_json.cpp).
[[nodiscard]] nlohmann::json encode_particle_emitter(const renderer::ParticleEmitterSettings& s);
void decode_particle_emitter(core::JsonFields& f, renderer::ParticleEmitterSettings& s);

} // namespace engine::scene::detail
